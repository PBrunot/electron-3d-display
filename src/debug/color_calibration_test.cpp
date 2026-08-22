#include "debug/color_calibration_test.h"

#include <cstdio>

#include "esp_log.h"
#include "render/font.h"
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

// Order matters: the four pure single-channel swatches first (a known-good reference -- if
// these read wrong, something fundamental changed and the compound ones below are moot).
// Then the actual orbital phase-color pair (orange/blue, see orbital_library.h) -- orange
// is the compound case (R+G+B all nonzero) the pure swatches don't exercise. Then two
// single-channel-dropped variants of orange, to bisect which channel combination breaks if
// the full orange swatch ever does.
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
        for (int y = 0; y < Display::kDisplayHeight; y++)
            for (int x = 0; x < Display::kDisplayWidth; x++)
                display.writePx(x, y, raw565);

        // Label in the opposite of the swatch's own color (readable against any fill) --
        // black text would vanish against a dark swatch, white against a bright one.
        uint16_t textColor = (int(s.r) + int(s.g) + int(s.b)) > 380 ? Display::kColorBlack : Display::kColorWhite;
        char hexLabel[16];
        std::snprintf(hexLabel, sizeof(hexLabel), "0x%04X", raw565);
        drawText(display, 10, 10, s.label, textColor, kFontLarge);
        drawText(display, 10, 10 + kFontLarge.height + 4, hexLabel, textColor, kFontLarge);
        display.presentFrame();

        vTaskDelay(pdMS_TO_TICKS(kSwatchHoldMs));
        index = (index + 1) % kSwatchCount;
    }
}
