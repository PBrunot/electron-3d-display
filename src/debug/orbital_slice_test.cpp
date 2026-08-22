#include "debug/orbital_slice_test.h"

#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_log.h"
#include "esp_timer.h"
#include "render/font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config/visual_constants.h" // kSliceIntroFrames, kSliceFadeOutFrames, kTitleTextX/Y, kTextColor
#include "views/orbital_slice.h"
#include "views/orbital_view.h"

static const char *kOrbitalSliceTestTag = "orbital_slice_test";

namespace
{
    // 2px, 2pz, 3dx2-y2, 4fz3, 1s, 2s -- one from each shape family (p, p, d, f, s, s),
    // per SLICE.md section 6.6; 2s added so the faint outer lobe after its radial node can be
    // eyeballed under the density-normalized brightness mapping. Indices into
    // orbital_library.h's kOrbitalLibrary.
    constexpr int kTestPresetIndices[] = {2, 4, 12, 14, 0, 1};
    constexpr int kTestPresetCount = int(sizeof(kTestPresetIndices) / sizeof(kTestPresetIndices[0]));

    constexpr int64_t kTestHoldUs = 3 * 1000 * 1000; // ~3s per preset, see orbital_slice_test.h

    void drawTestFrame(Display &display, const SliceTable &table, const char *title, orb_real_t fade)
    {
        display.waitForFlushDone();
        renderSliceFrame(display, table, fade);
        drawText(display, kTitleTextX, kTitleTextY, title, kTextColor, kFontHuge);
        // Same legend orbital_view.cpp's drawSliceOverlay() shows on the real device --
        // naming the density plot here too, so this quick-look harness matches what Right-hold
        // actually looks like.
        drawText(display, kTitleTextX, kTitleTextY + kFontHuge.height + 2, "densita di probabilita", kScaleBarColor,
                 kFontSmall);
        display.presentFrame();
    }
} // namespace

void runOrbitalSliceTest(Display &display)
{
    // Static PSRAM, not stack -- OrbitalPresetState/SliceTable are tens of KB, same rationale
    // as orbital_view.cpp's runOrbitalView()/runSliceSequence() (this test reuses
    // OrbitalPresetState::load() purely for its already-tested radialCoeff/legendreCoeff/rRef
    // build, not to run the full cloud viewer).
    static EXT_RAM_BSS_ATTR OrbitalPresetState preset;
    static EXT_RAM_BSS_ATTR SliceTable table;

    while (true)
    {
        for (int i = 0; i < kTestPresetCount; i++)
        {
            int index = kTestPresetIndices[i];
            preset.load(index);

            int64_t buildStartUs = esp_timer_get_time();
            buildSliceTable(preset.resample.n, preset.resample.ell, preset.resample.m, preset.resample.radialCoeff,
                            preset.resample.legendreCoeff, preset.rRef, &table);
            int64_t buildMs = (esp_timer_get_time() - buildStartUs) / 1000;
            ESP_LOGI(kOrbitalSliceTestTag, "%s: slice built in %lldms", preset.title, buildMs);

            for (int f = 0; f < kSliceIntroFrames; f++)
            {
                orb_real_t fade =
                    kSliceIntroFrames > 1 ? orb_real_t(f) / orb_real_t(kSliceIntroFrames - 1) : orb_real_t(1);
                drawTestFrame(display, table, preset.title, fade);
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            int64_t holdStartUs = esp_timer_get_time();
            while (esp_timer_get_time() - holdStartUs < kTestHoldUs)
            {
                drawTestFrame(display, table, preset.title, orb_real_t(1));
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            for (int f = kSliceFadeOutFrames; f >= 0; f--)
            {
                orb_real_t fade =
                    kSliceFadeOutFrames > 0 ? orb_real_t(f) / orb_real_t(kSliceFadeOutFrames) : orb_real_t(0);
                drawTestFrame(display, table, preset.title, fade);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
    }
}
