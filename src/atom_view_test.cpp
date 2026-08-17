#include "atom_view_test.h"

#include <cstdio>
#include <cstring>

#include "atom_cloud.h"
#include "camera.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "overlay.h"

static const char *kAtomViewTestTag = "atom_view_test";

// Full multi-electron atom point cloud (atom_cloud.h): angular tables are compile-time
// embedded (angular_library.h, confirmed working the same way kOrbital1sSampler was --
// xtensa-esp32s3-elf-nm showed it in .rodata, no runtime initializer call); each
// subshell's radial table is built at RUNTIME from slater.h's Z_eff model when this atom
// is picked (there are too many (n,ell,Z_eff) combinations across 118 elements to embed
// them all -- see pointcloud.h's angular/radial-split comment). Iron (Z=26) matches
// ATOMS.md's worked example (3d Z_eff=11.180). Switching elements is future UI work (M4).
static constexpr int kAtomicNumber = 26; // Fe

static constexpr int kNumPoints = 5000;
static constexpr uint32_t kRngSeed = 12345;
// Orbital-space-units-to-pixels scale. A full atom's outer/valence subshell reaches much
// further out (in Bohr radii) than a bare 1s cloud does, so this is a rough starting
// guess -- this legacy M1/M2 placeholder was superseded by atom_view.h/.cpp's
// scaleForAtom() (M4), which renormalizes every element to the same on-screen radius.
static constexpr orb_real_t kScale = orb_real_t(10);

static constexpr uint16_t kProtonColor = Display::packColor565(255, 0, 0);

void runAtomViewTest(Display &display)
{
    // Sample the point cloud ONCE (builds every occupied subshell's radial table as it
    // goes); only the rotation/projection below runs every frame (matches CLAUDE.md §5:
    // generate points once per orbital/atom, then just rotate/reproject -- not per-frame
    // resampling).
    static AtomPoint points[kNumPoints];
    int64_t buildStartUs = esp_timer_get_time();
    ElectronConfig config = buildAtomPointCloud(kAtomicNumber, points, kNumPoints, kRngSeed);
    int64_t buildMs = (esp_timer_get_time() - buildStartUs) / 1000;

    ESP_LOGI(kAtomViewTestTag, "%s (Z=%d): %d subshells, %d points, built in %lldms", elementSymbol(kAtomicNumber),
             kAtomicNumber, config.count, kNumPoints, buildMs);
    for (int i = 0; i < config.count; i++)
    {
        ESP_LOGI(kAtomViewTestTag, "  %d%c%d", config.subshells[i].n, subshellLabelChar(config.subshells[i].ell),
                 config.subshells[i].occ);
    }

    char titleText[32];
    std::snprintf(titleText, sizeof(titleText), "%s (Z=%d)", elementSymbol(kAtomicNumber), kAtomicNumber);
    constexpr uint16_t kTextColor = Display::kColorWhite;
    constexpr uint16_t kScaleBarColor = Display::packColor565(210, 210, 210);

    // FPS benchmark: no fixed per-frame delay (a fixed vTaskDelay(33) would just measure
    // the delay, not the hardware's real ceiling) -- vTaskDelay(1) below is the minimum
    // yield to keep FreeRTOS's idle/watchdog task fed, ~10ms/tick at the default 100Hz
    // tick rate, i.e. a ~100 FPS ceiling from the delay alone -- well above the ~40 FPS
    // theoretical ceiling from the SPI transfer itself (CLAUDE.md §6: 240x240x16bit at
    // 40MHz ~= 23ms/frame), so the delay should never be the bottleneck we're measuring.
    constexpr int kFpsReportEveryNFrames = 60;
    int64_t reportWindowStartUs = esp_timer_get_time();
    int framesSinceReport = 0;

    // Per-phase profiling: split each frame into (a) time blocked in waitForFlushDone()
    // waiting on the PREVIOUS frame's SPI DMA to finish, and (b) CPU render time (memset +
    // rotate/project/rasterize + text). presentFrame() itself just queues DMA and returns,
    // so it's folded into (b) rather than tracked separately. Summed and averaged over the
    // same window as the FPS report so we can see which phase actually dominates the ~28ms
    // budget instead of guessing from config changes alone.
    int64_t waitUsAccum = 0;
    int64_t renderUsAccum = 0;

    CameraState camera;
    while (1)
    {
        int64_t tBeforeWait = esp_timer_get_time();
        display.waitForFlushDone(); // wait for the previous frame's DMA to finish before overwriting the buffer
        int64_t tAfterWait = esp_timer_get_time();

        std::memset(display.getFrameBuf(), 0, Display::kDisplayWidth * Display::kDisplayHeight * sizeof(uint16_t));

        RotationTrig trig = computeRotationTrig(camera);
        renderPointsUniform(display.getFrameBuf(), points, kNumPoints, Display::kColorWhite, trig, kScale);
        drawProtonMarker(display.getFrameBuf(), kProtonColor);

        drawText(display.getFrameBuf(), kTitleTextX, kTitleTextY, titleText, kTextColor, kFontLarge);
        drawScaleBar(display.getFrameBuf(), kScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

        display.presentFrame();
        int64_t tAfterPresent = esp_timer_get_time();
        stepCamera(&camera);

        waitUsAccum += tAfterWait - tBeforeWait;
        renderUsAccum += tAfterPresent - tAfterWait;

        framesSinceReport++;
        if (framesSinceReport >= kFpsReportEveryNFrames)
        {
            int64_t nowUs = esp_timer_get_time();
            double elapsedS = double(nowUs - reportWindowStartUs) / 1e6;
            double fps = double(framesSinceReport) / elapsedS;
            ESP_LOGI(kAtomViewTestTag, "FPS: %.1f (%d frames / %.3fs, %d points/frame)", fps, framesSinceReport,
                     elapsedS, kNumPoints);
            ESP_LOGI(kAtomViewTestTag, "  avg wait(prev DMA)=%.2fms avg render(CPU)=%.2fms",
                     double(waitUsAccum) / framesSinceReport / 1000.0,
                     double(renderUsAccum) / framesSinceReport / 1000.0);
            reportWindowStartUs = nowUs;
            framesSinceReport = 0;
            waitUsAccum = 0;
            renderUsAccum = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
