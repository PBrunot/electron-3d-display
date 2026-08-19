// Exactly one of the five #define toggles below may be active at a time; with none
// defined, app_main() boots the runtime chooser menu (the real default, see chooser.h) --
// tilt UP launches the orbital viewer, tilt DOWN the element viewer, matching
// micropython/chooser.py's role. COLOR_TEST is currently active below for direct
// hardware color-mapping diagnosis (see color_calibration_test.h) -- comment it out
// (and uncomment ATOM_VIEW) to reach the chooser instead.

// #define ATOM_VALIDATION_TEST
// #define ATOM_VIEW_TEST
// #define ATOM_VIEW
// #define COLOR_TEST
// #define BENCHMARK_TEST

#include "ux/chooser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ux/imu.h"
#include "debug/screenshot_console.h"
#include "render/splash_bitmap.h"
#include "config/hardware_constants.h"
#include "config/visual_constants.h" // kSplashHoldMs
#include "ux/tilt_gesture.h"

#ifdef ATOM_VALIDATION_TEST
#include "debug/atom_validation_test.h"
#endif

#ifdef COLOR_TEST
#include "debug/color_calibration_test.h"
#endif

#ifdef BENCHMARK_TEST
#include "debug/benchmark_test.h"
#endif

#include "debug/atom_view_test.h"
#include "views/atom_view.h"

static const char *kMainTag = "main";

extern "C" void app_main(void)
{
#ifdef ATOM_VALIDATION_TEST
    while (1)
    {
        runAtomValidationTest();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#elif defined(COLOR_TEST)
    Display display{};
    runColorCalibrationTest(display); // never returns
#elif defined(BENCHMARK_TEST)
    Display display{};
    runBenchmarkTest(display);
    ESP_LOGI(kMainTag, "benchmark finished -- capture via `pio device monitor`, watch for BENCH,DONE; "
                        "reflash without BENCHMARK_TEST to return to normal boot");
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
#else
    Display display{};
    startScreenshotConsole(display); // 's'/'l'/SS_GET/SS_DEL over the console -- see screenshot_console.h

    display.waitForFlushDone();
    drawSplashScreen(display.getFrameBuf());
    display.presentFrame();
    vTaskDelay(pdMS_TO_TICKS(kSplashHoldMs));

    Qmi8658 imu{};
    TiltGestureDetector tilt{imu};

    if (imu.checkPlanarAtBoot())
    {
        ESP_LOGI(kMainTag, "boot: planar check OK, using hardcoded calibration");
        tilt.setBaseline(kDefaultBaselineX, kDefaultBaselineY, kDefaultBaselineZ);
        tilt.setMapping(kDefaultDirRefLeftX, kDefaultDirRefLeftY, kDefaultDirRefLeftZ, TiltDirection::kLeft);
        tilt.setMapping(kDefaultDirRefRightX, kDefaultDirRefRightY, kDefaultDirRefRightZ, TiltDirection::kRight);
        tilt.setMapping(kDefaultDirRefUpX, kDefaultDirRefUpY, kDefaultDirRefUpZ, TiltDirection::kUp);
        tilt.setMapping(kDefaultDirRefDownX, kDefaultDirRefDownY, kDefaultDirRefDownZ, TiltDirection::kDown);
    }
    else
    {
        ESP_LOGI(kMainTag, "boot: not planar -- forcing calibration in 2s (power on upside-down to force this)");
        vTaskDelay(pdMS_TO_TICKS(2000));
        tilt.calibrate();
        calibrateDirections(display, tilt);
        tilt.logCalibrationForHardcode();
    }
    runChooser(display, tilt);
#endif
}
