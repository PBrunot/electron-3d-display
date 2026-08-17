// ESP-IDF esp_lcd SPI driver for the Waveshare ESP32-S3-LCD-1.3 (ST7789V2, 240x240).
// Pinout and color-order/mirror correction per CLAUDE.md sections 2/3.
// Display bring-up lives in display.h/.cpp; the tumble camera lives in camera.h; the
// hydrogen orbital viewer (this build's default) lives in orbital_view.h/.cpp; the two
// standalone test builds below live in atom_view_test.h/.cpp and
// atom_validation_test.h/.cpp -- see those files' headers for what moved where and why
// (app-layer parity milestones M1-M3, see the plan this implements in the conversation
// that added it).
//
// Exactly one of the two #define toggles below may be active at a time; with neither
// defined, app_main() boots the orbital viewer (the real default).

// #define ATOM_VALIDATION_TEST
#define ATOM_VIEW_TEST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_view.h"

#ifdef ATOM_VALIDATION_TEST
#include "atom_validation_test.h"
#endif

#ifdef ATOM_VIEW_TEST
#include "atom_view_test.h"
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
