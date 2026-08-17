// Exactly one of the four #define toggles below may be active at a time; with none
// defined, app_main() boots the orbital viewer (the real default).

// #define ATOM_VALIDATION_TEST
// #define ATOM_VIEW_TEST
#define ATOM_VIEW
// #define COLOR_TEST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_view.h"

#ifdef ATOM_VALIDATION_TEST
#include "atom_validation_test.h"
#endif

#ifdef ATOM_VIEW_TEST
#include "atom_view_test.h"
#endif

#ifdef ATOM_VIEW
#include "atom_view.h"
#endif

#ifdef COLOR_TEST
#include "color_test.h"
#endif

extern "C" void app_main(void) {
#ifdef ATOM_VALIDATION_TEST
    while (1) {
        runAtomValidationTest();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#elif defined(ATOM_VIEW_TEST)
    Display display{};
    runAtomViewTest(display);
#elif defined(ATOM_VIEW)
    Display display{};
    runAtomView(display);
#elif defined(COLOR_TEST)
    Display display{};
    runColorTest(display);
#else
    Display display{};
    runOrbitalView(display);
#endif
}
