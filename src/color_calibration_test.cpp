#include "color_calibration_test.h"

#include <algorithm>
#include <cstdio>

#include "esp_log.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *kColorCalibTag = "color_calib_test";

namespace
{
struct Swatch
{
    const char *label;
    uint8_t r, g, b; // physical-intent RGB fed to Display::packColor565()
};

// Order matters: the four pure single-channel swatches first (CLAUDE.md's already-
// verified-by-hand reference -- if these don't match CLAUDE.md's documented expectation,
// something ELSE changed since that verification and the compound ones below are moot).
// Then the exact orbital phase-color pair (orange/plain-blue -- orbital blue is now plain
// (0,0,255), matching orbital_library.h/cloud_common.py; orange is still R+G+B all nonzero,
// the compound case CLAUDE.md's four-pure-color test never actually exercised). Then two
// single-channel-dropped variants of orange to bisect WHICH channel combination breaks.
constexpr Swatch kSwatches[] = {
    {"RED (pure)", 255, 0, 0},
    {"GREEN (pure)", 0, 255, 0},
    {"BLUE (pure)", 0, 0, 255},
    {"YELLOW (pure)", 255, 255, 0},
    {"ORBITAL ORANGE", 255, 120, 40},
    {"ORBITAL BLUE (plain)", 0, 0, 255},
    {"ORANGE, no G (255,0,40)", 255, 0, 40},
    {"ORANGE, no B (255,120,0)", 255, 120, 0},
};
constexpr int kSwatchCount = sizeof(kSwatches) / sizeof(kSwatches[0]);

constexpr uint32_t kSwatchHoldMs = 3000;
} // namespace

void runColorCalibrationTest(Display &display)
{
    ESP_LOGI(kColorCalibTag, "%d swatches, %ums each, looping forever -- report which PHYSICAL", kSwatchCount,
             kSwatchHoldMs);
    ESP_LOGI(kColorCalibTag, "color each one actually shows as (not just the label on screen)");

    int index = 0;
    while (1)
    {
        const Swatch &s = kSwatches[index];
        uint16_t raw565 = Display::packColor565(s.r, s.g, s.b);

        ESP_LOGI(kColorCalibTag, "[%d/%d] %s -- intent RGB(%d,%d,%d) -> raw565=0x%04X", index + 1, kSwatchCount,
                 s.label, s.r, s.g, s.b, raw565);

        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, raw565);

        // Label in the opposite of the swatch's own color (readable against any fill) --
        // black text would vanish against a dark swatch, white against a bright one.
        uint16_t textColor = (int(s.r) + int(s.g) + int(s.b)) > 380 ? Display::kColorBlack : Display::kColorWhite;
        char hexLabel[16];
        std::snprintf(hexLabel, sizeof(hexLabel), "0x%04X", raw565);
        drawText(frameBuf, 10, 10, s.label, textColor, kFontLarge);
        drawText(frameBuf, 10, 10 + kFontLarge.height + 4, hexLabel, textColor, kFontLarge);
        display.presentFrame();

        vTaskDelay(pdMS_TO_TICKS(kSwatchHoldMs));
        index = (index + 1) % kSwatchCount;
    }
}
