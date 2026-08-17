#include "color_test.h"

#include "camera.h" // kElectronAlphaQ8, kPersistenceKeepQ8
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* kColorTestTag = "color_test";

namespace {
struct TestColor {
    const char* name;
    uint8_t r, g, b;
};

// Matches CLAUDE.md's verified full-screen test constants (section 2, "scambio G/B") --
// physical-intent RGB in, packColor565() must reproduce exactly these colors on the
// panel: red/green/blue/yellow unmixed, plus white/black as trivial sanity checks.
constexpr TestColor kTestColors[] = {
    {"RED", 255, 0, 0},      {"GREEN", 0, 255, 0},     {"BLUE", 0, 0, 255},
    {"YELLOW", 255, 255, 0}, {"WHITE", 255, 255, 255}, {"BLACK", 0, 0, 0},
};
} // namespace

void runColorTest(Display& display) {
    ESP_LOGI(kColorTestTag, "color test: %d colors, ~2s each -- compare against CLAUDE.md's physical colors",
             int(sizeof(kTestColors) / sizeof(kTestColors[0])));

    while (true) {
        for (const TestColor& tc : kTestColors) {
            uint16_t packed = Display::packColor565(tc.r, tc.g, tc.b);

            // Round-trip/derived values are LOG-ONLY sanity checks, not drawn anywhere --
            // if these don't match what the eye sees on the panel, the bug is in
            // pack/unpack/blend/fade math, not in the panel's physical wiring.
            uint8_t ur, ug, ub;
            Display::unpackColor565(packed, &ur, &ug, &ub);
            uint16_t blended = Display::blendColor565(Display::kColorBlack, packed, kElectronAlphaQ8);
            uint16_t faded = Display::fadeColor565(packed, kPersistenceKeepQ8);

            ESP_LOGI(kColorTestTag,
                     "%-6s rgb=(%3d,%3d,%3d) packed=0x%04X unpacked=(%3d,%3d,%3d) "
                     "blend(black->this,a=0.8)=0x%04X fade(this,keep=0.39)=0x%04X",
                     tc.name, tc.r, tc.g, tc.b, unsigned(packed), ur, ug, ub, unsigned(blended), unsigned(faded));

            display.waitForFlushDone();
            uint16_t* frameBuf = display.getFrameBuf();
            for (int i = 0; i < Display::kDisplayWidth * Display::kDisplayHeight; i++)
                frameBuf[i] = packed;
            display.presentFrame();

            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}
