#define ATOM_VIEW_TEST

// ESP-IDF esp_lcd SPI driver for the Waveshare ESP32-S3-LCD-1.3 (ST7789V2, 240x240).
// Pinout and color-order/mirror correction per CLAUDE.md sections 2/3.
// Display bring-up lives in display.h/.cpp; the tumble camera lives in camera.h; the
// hydrogen orbital viewer (this build's default) lives in orbital_view.h/.cpp -- see
// those files' headers for what moved where and why (app-layer parity milestones M1-M3,
// see the plan this implements in the conversation that added it).

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_view.h"

// Uncomment to build a validation-data-dump instead of the interactive viewer: prints
// electron-configuration/Z_eff/point-sample data for a curated element set as tagged CSV
// log lines, for comparison against tools/orbitals_host/gen_atom_reference.py's host
// reference (run under the real MicroPython unix port -- see that script's docstring and
// tools/orbitals_host/compare_atom.py). Skips LCD init entirely; capture with e.g.
// `pio device monitor > capture.log`, then run compare_atom.py on it. See the plan this
// implements in the conversation that added it, and slater.h/atom_cloud.h for the ported
// model. Comment back out (and rebuild/reflash) to return to the interactive viewer.
// #define ATOM_VALIDATION_TEST

// Uncomment to boot into the M1/M2 single-hardcoded-atom test build instead of the
// orbital viewer -- kept around as a standalone alternate entry point (same role as
// micropython/atom_view.py before chooser.py existed: "run this instead of main.py's
// default to get the other one"), useful for comparing the two render paths or debugging
// atom_cloud.h in isolation before atom_view.h/.cpp (M4) replaces this block outright.
// #define ATOM_VIEW_TEST

#if defined(ATOM_VALIDATION_TEST) || defined(ATOM_VIEW_TEST)
#include "atom_cloud.h"
#endif

#ifdef ATOM_VIEW_TEST
#include "camera.h"
#include "display.h"
#include "esp_timer.h"
#include "font.h"
#include "overlay.h"

#include <cstdio>
#include <cstring>
#endif

static const char* TAG = "orbital_test";

#ifdef ATOM_VALIDATION_TEST
// MUST match tools/orbitals_host/gen_atom_reference.py's ATOM_TEST_CASES/SEED/POINTS_PER_CASE
// exactly, or the two sides aren't comparing the same thing.
constexpr int kValidationZs[] = {1, 2, 6, 10, 24, 26, 46, 58}; // H, He, C, Ne, Cr, Fe, Pd, Ce
constexpr uint32_t kValidationSeed = 12345;
constexpr int kValidationPoints = 50;

static void runAtomValidationTest() {
    static AtomPoint points[kValidationPoints];

    for (int zi = 0; zi < int(sizeof(kValidationZs) / sizeof(kValidationZs[0])); zi++) {
        int z = kValidationZs[zi];
        const char *symbol = elementSymbol(z);
        ElectronConfig config = buildAtomPointCloud(z, points, kValidationPoints, kValidationSeed);

        for (int i = 0; i < config.count; i++) {
            ESP_LOGI(TAG, "ATOMTEST,CONFIG,%s,%d,%d,%d", symbol, config.subshells[i].n, config.subshells[i].ell,
                     config.subshells[i].occ);
        }
        for (int i = 0; i < config.count; i++) {
            int n = config.subshells[i].n, ell = config.subshells[i].ell;
            orb_real_t zEff = zEffRadial(z, config, n, ell);
            ESP_LOGI(TAG, "ATOMTEST,ZEFF,%s,%d,%d,%.17g", symbol, n, ell, double(zEff));
        }
        for (int i = 0; i < kValidationPoints; i++) {
            ESP_LOGI(TAG, "ATOMTEST,POINT,%s,%d,%.17g,%.17g,%.17g", symbol, i, double(points[i].x),
                     double(points[i].y), double(points[i].z));
        }
    }
    ESP_LOGI(TAG, "ATOMTEST,DONE");
}
#endif

#ifdef ATOM_VIEW_TEST

// Full multi-electron atom point cloud (atom_cloud.h): angular tables are compile-time
// embedded (angular_library.h, confirmed working the same way kOrbital1sSampler was --
// xtensa-esp32s3-elf-nm showed it in .rodata, no runtime initializer call); each
// subshell's radial table is built at RUNTIME from slater.h's Z_eff model when this atom
// is picked (there are too many (n,ell,Z_eff) combinations across 118 elements to embed
// them all -- see pointcloud.h's angular/radial-split comment). Iron (Z=26) matches
// ATOMS.md's worked example (3d Z_eff=11.180). Switching elements is future UI work (M4).
constexpr int kAtomicNumber = 26; // Fe

constexpr int kNumPoints = 5000;
constexpr uint32_t kRngSeed = 12345;
// Orbital-space-units-to-pixels scale. A full atom's outer/valence subshell reaches much
// further out (in Bohr radii) than a bare 1s cloud does, so this is a rough starting
// guess, NOT calibrated the way ATOMS.md's pixels_per_bohr_for_canvas() is on the PC
// side -- almost certainly needs visual tuning once actually seen on the device. Real
// per-element calibration (scaleForAtom()/pixelsPerBohrForCanvas()) lands in M4.
constexpr orb_real_t kScale = orb_real_t(10);

constexpr uint16_t kProtonColor = packColor565(255, 0, 0);

static void runAtomViewTest() {
    Display display = initDisplay();

    // Sample the point cloud ONCE (builds every occupied subshell's radial table as it
    // goes); only the rotation/projection below runs every frame (matches CLAUDE.md §5:
    // generate points once per orbital/atom, then just rotate/reproject -- not per-frame
    // resampling).
    static AtomPoint points[kNumPoints];
    int64_t buildStartUs = esp_timer_get_time();
    ElectronConfig config = buildAtomPointCloud(kAtomicNumber, points, kNumPoints, kRngSeed);
    int64_t buildMs = (esp_timer_get_time() - buildStartUs) / 1000;

    ESP_LOGI(TAG, "%s (Z=%d): %d subshells, %d points, built in %lldms", elementSymbol(kAtomicNumber),
             kAtomicNumber, config.count, kNumPoints, buildMs);
    for (int i = 0; i < config.count; i++) {
        ESP_LOGI(TAG, "  %d%c%d", config.subshells[i].n, subshellLabelChar(config.subshells[i].ell),
                 config.subshells[i].occ);
    }

    char titleText[32];
    std::snprintf(titleText, sizeof(titleText), "%s (Z=%d)", elementSymbol(kAtomicNumber), kAtomicNumber);
    constexpr uint16_t kTextColor = kColorWhite;
    constexpr uint16_t kScaleBarColor = packColor565(210, 210, 210);

    // FPS benchmark: no fixed per-frame delay (a fixed vTaskDelay(33) would just measure
    // the delay, not the hardware's real ceiling) -- vTaskDelay(1) below is the minimum
    // yield to keep FreeRTOS's idle/watchdog task fed, ~10ms/tick at the default 100Hz
    // tick rate, i.e. a ~100 FPS ceiling from the delay alone -- well above the ~40 FPS
    // theoretical ceiling from the SPI transfer itself (CLAUDE.md §6: 240x240x16bit at
    // 40MHz ~= 23ms/frame), so the delay should never be the bottleneck we're measuring.
    constexpr int kFpsReportEveryNFrames = 60;
    int64_t reportWindowStartUs = esp_timer_get_time();
    int framesSinceReport = 0;
    char fpsText[16] = "FPS: --";

    CameraState camera;
    while (1) {
        std::memset(display.frameBuf, 0, kDisplayWidth * kDisplayHeight * sizeof(uint16_t));

        RotationTrig trig = computeRotationTrig(camera);
        renderPointsUniform(display.frameBuf, points, kNumPoints, kColorWhite, trig, kScale);
        drawProtonMarker(display.frameBuf, kProtonColor);

        drawText(display.frameBuf, kTitleTextX, kTitleTextY, titleText, kTextColor);
        drawText(display.frameBuf, kFpsTextX, kFpsTextY, fpsText, kTextColor);
        drawScaleBar(display.frameBuf, kScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

        presentFrame(display);
        stepCamera(&camera);

        framesSinceReport++;
        if (framesSinceReport >= kFpsReportEveryNFrames) {
            int64_t nowUs = esp_timer_get_time();
            double elapsedS = double(nowUs - reportWindowStartUs) / 1e6;
            double fps = double(framesSinceReport) / elapsedS;
            std::snprintf(fpsText, sizeof(fpsText), "FPS: %.1f", fps);
            ESP_LOGI(TAG, "FPS: %.1f (%d frames / %.3fs, %d points/frame)", fps, framesSinceReport, elapsedS,
                     kNumPoints);
            reportWindowStartUs = nowUs;
            framesSinceReport = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
#endif

extern "C" void app_main(void) {
#ifdef ATOM_VALIDATION_TEST
    while (1) {
        runAtomValidationTest();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#elif defined(ATOM_VIEW_TEST)
    runAtomViewTest();
#else
    runOrbitalView();
#endif
}
