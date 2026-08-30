// Exactly one of the six #define toggles below may be active at a time; with none
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
// #define GIF_CAPTURE_TEST

#include "ux/chooser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h" // CONFIG_IDF_TARGET_ESP32

#include "ux/imu.h"
#include "debug/screenshot_console.h"
#include "render/splash_bitmap.h"
#include "config/hardware_constants.h"
#include "config/visual_constants.h" // kSplashHoldMs
#include "ux/tilt_gesture.h"
#include "util/storage_mount.h"
#include "views/orbital_view.h" // CYD boot fallback -- see the CONFIG_IDF_TARGET_ESP32 branch below
#include "debug/benchmark_test.h"
#include "debug/gif_capture_test.h"

#ifdef ATOM_VALIDATION_TEST
#include "debug/atom_validation_test.h"
#endif

#ifdef COLOR_TEST
#include "debug/color_calibration_test.h"
#endif

#include "debug/atom_view_test.h"
#include "views/atom_view.h"

static const char *kMainTag = "main";

extern "C" void app_main(void)
{
    logMemory("startup");
    // Mount /storage before any Display is constructed: Display's frame buffer allocation
    // fragments the DMA-capable heap into many small blocks (display.cpp), and SPIFFS's own
    // cache buffer allocation was landing in that fragmented heap and failing every time
    // (CYD-branch.md). Mounting here, while the heap is still whole, makes every later
    // ensureStorageMounted() call (splash_bitmap.cpp, orbital_library.cpp, hfs_radial.cpp) a
    // cheap no-op (ESP_ERR_INVALID_STATE) instead of a retry that fails the same way.
    ensureStorageMounted();
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
#elif defined(GIF_CAPTURE_TEST)
    Display display{};
    startScreenshotConsole(display); // so pc/pull_screenshots.py can SS_LIST/SS_GET the saved frames afterward
    runGifCaptureTest(display);
    ESP_LOGI(kMainTag, "gif capture finished -- pull frames via `python3 pc/pull_screenshots.py --all`, "
                       "watch for GIF,DONE; reflash without GIF_CAPTURE_TEST to return to normal boot");
    while (1)
        vTaskDelay(pdMS_TO_TICKS(1000));
#else
    Display display{};
#if !CONFIG_IDF_TARGET_ESP32
    startScreenshotConsole(display); // 's'/'l'/SS_GET/SS_DEL over the console -- see screenshot_console.h
#endif
    // CYD: screenshot console skipped entirely (not just its batch command) -- no reachable
    // call site left into screenshot_console.cpp/screenshot.cpp/screenshot_batch.cpp, so the
    // linker's --gc-sections drops all three translation units (CYD-branch.md). Recovers the
    // ~53KB of internal SRAM that
    // screenshot_batch.cpp's captureOrbitals()/captureAllPresets() static scratch (tens of KB
    // of OrbitalPresetState/AtomPresetState duplicates of the live view's own) reserved for a
    // batch-capture feature that was already a no-op here (it heap_caps_mallocs with
    // MALLOC_CAP_SPIRAM, which always fails with no PSRAM) -- see config/visual_constants.h's
    // kOrbitalNumPoints comment. That RAM instead goes toward restoring full 240x320 resolution
    // (Display::kDisplayWidth/Height) and a higher point count.

    display.waitForFlushDone();
    drawSplashScreen(display);
    display.presentFrame();
    vTaskDelay(pdMS_TO_TICKS(kSplashHoldMs));

    Qmi8658 imu{};
    TiltGestureDetector tilt{imu};

#if CONFIG_IDF_TARGET_ESP32
    // CYD has no IMU (Qmi8658 is a no-op shim here, see ux/imu.cpp) -- skip planar
    // check/calibration entirely rather than falling into the "not planar" branch below:
    // checkPlanarAtBoot() would always read zero samples and report "not planar", and
    // calibrateDirections() (ux/chooser.cpp) waits in an unbounded loop for a tilt gesture
    // that can never arrive on this board, hanging boot forever instead of just failing to
    // navigate. Tilt-based menu navigation is a known non-functional gap on CYD until a
    // replacement input (touch/BOOT button) is decided -- see CYD-branch.md.
    ESP_LOGW(kMainTag, "CYD build: no IMU, skipping tilt calibration -- chooser will not respond to tilt");
    logMemory("startup: chooser");
    // TEMPORARY testing convenience while CYD has no working input (see the tilt-skip note
    // above): the real chooser menu is inert here (nothing can ever confirm a tilt gesture),
    // so jump straight into the orbital viewer after a short delay instead of sitting on an
    // unusable menu screen. Remove once a real input method (touch/BOOT button) replaces
    // tilt navigation on this board -- see CYD-branch.md.
    ESP_LOGW(kMainTag, "CYD build: chooser has no working input -- auto-launching orbital viewer in 5s");
    vTaskDelay(pdMS_TO_TICKS(5000));
    // Neither view can ever return via its normal left-tilt-hold exit here (no IMU), so this
    // loop -- not chooser.cpp's -- is what stands in for its idle orbital/element coin-flip:
    // each view's own idle jump has a kViewCrossSwitchProbability chance to return instead of
    // picking another preset, which is what actually alternates the two below.
    while (true)
    {
        runOrbitalView(display, tilt);
        runAtomView(display, tilt);
    }
#else
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
    logMemory("startup: chooser");
    runChooser(display, tilt);
#endif
#endif
}
