// Exactly one of the four #define toggles below may be active at a time; with none
// defined, app_main() boots the runtime chooser menu (the real default, see chooser.h) --
// tilt UP launches the orbital viewer, tilt DOWN the element viewer, matching
// micropython/chooser.py's role. ATOM_VIEW is currently active below for direct testing --
// comment it out to reach the chooser instead.

// #define ATOM_VALIDATION_TEST
// #define ATOM_VIEW_TEST
#define ATOM_VIEW
// #define COLOR_TEST

#include "chooser.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "tilt_gesture.h"

#ifdef ATOM_VALIDATION_TEST
#include "atom_validation_test.h"
#endif

#include "atom_view_test.h"
#include "atom_view.h"

extern "C" void app_main(void)
{
#ifdef ATOM_VALIDATION_TEST
    while (1)
    {
        runAtomValidationTest();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#else
    Display display{};
    Qmi8658 imu{};
    TiltGestureDetector tilt{imu};
    tilt.calibrate();
    calibrateDirections(display, tilt);
    runChooser(display, tilt);
#endif
}
