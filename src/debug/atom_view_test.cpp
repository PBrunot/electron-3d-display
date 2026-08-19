#include "debug/atom_view_test.h"

#include <cstdio>
#include <cstring>

#include "physics/atom_cloud.h"
#include "render/camera.h"
#include "render/display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "render/font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "render/overlay.h"

static const char *kAtomViewTestTag = "atom_view_test";

// ============================================================================================
// Tunable constants
// ============================================================================================

/// Element shown by this fixed benchmark/smoke-test view. Superseded by atom_view.h/.cpp for
/// the real element-switching UI (M4); kept here only as a standing FPS/profiling harness.
static constexpr int kAtomicNumber = 26; // Fe

static constexpr int kNumPoints = 5000;
static constexpr uint32_t kRngSeed = 12345;
/// Orbital-space-units-to-pixels scale. A rough fixed guess, not renormalized per element the
/// way atom_view.h/.cpp's scaleForAtom() is -- fine for this single-element benchmark.
static constexpr orb_real_t kScale = orb_real_t(10);

/// Frame window size for the FPS/phase-timing log lines below.
constexpr int kFpsReportEveryNFrames = 60;

// ============================================================================================

void runAtomViewTest(Display &display)
{
    // Sample the point cloud ONCE (builds every occupied subshell's radial table as it goes);
    // only the rotation/projection below runs every frame.
    static AtomPoint points[kNumPoints];
    int64_t buildStartUs = esp_timer_get_time();
    AtomSubshellRange ranges[kMaxConfigSubshells];
    int rangeCount = 0;
    ElectronConfig config = buildAtomPointCloud(kAtomicNumber, points, kNumPoints, kRngSeed, ranges, &rangeCount);
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
    constexpr uint16_t kTestScaleBarColor = Display::packColor565(210, 210, 210); // slightly dimmer than kTextColor

    // FPS benchmark: no fixed per-frame delay (a fixed vTaskDelay(33) would just measure the
    // delay, not the hardware's real ceiling) -- vTaskDelay(1) below is the minimum yield to
    // keep FreeRTOS's idle/watchdog task fed, well above the SPI-transfer-limited theoretical
    // FPS ceiling (see display.cpp's LCD_PIXEL_CLOCK_HZ), so the delay should never be the
    // bottleneck being measured.
    int64_t reportWindowStartUs = esp_timer_get_time();
    int framesSinceReport = 0;

    // Per-phase profiling: split each frame into (a) time blocked in waitForFlushDone()
    // waiting on the PREVIOUS frame's SPI DMA to finish, and (b) CPU render time (memset +
    // rotate/project/rasterize + text). presentFrame() itself just queues DMA and returns, so
    // it's folded into (b) rather than tracked separately.
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
        drawScaleBar(display.getFrameBuf(), kScale / kPmPerBohr, "pm", kTestScaleBarColor, kTextColor);

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
