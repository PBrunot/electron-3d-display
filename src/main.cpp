// Boot mode is selected by kBootMode below: kNormal boots the runtime chooser menu (the real
// default, see chooser.h) -- tilt UP launches the orbital viewer, tilt DOWN the element viewer,
// matching micropython/chooser.py's role. Any other value runs that diagnostic instead and
// never reaches the chooser; see each one's own header for what it does and how to read its
// output.

#include "ux/chooser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ux/imu.h"
#include "ux/orientation_tracker.h"
#include "ux/touch.h"
#include "ux/touch_gesture.h"
#include "config/board.h" // kIsCYD
#include "debug/atom_validation_test.h"
#include "debug/benchmark_test.h"
#include "debug/color_calibration_test.h"
#include "debug/gif_capture_test.h"
#include "debug/screenshot_console.h"
#include "render/splash_bitmap.h"
#include "config/hardware_constants.h"
#include "config/network_constants.h" // kWebRemoteEnabled
#include "net/web_remote.h"
#include "config/visual_constants.h" // kSplashHoldMs
#include "ux/tilt_gesture.h"
#include "util/storage_mount.h"

constexpr const char *kMainTag = "main";

enum class BootMode
{
    kNormal,             // runtime chooser menu -- the real default
    kAtomValidationTest, // debug/atom_validation_test.h
    kColorTest,          // debug/color_calibration_test.h
    kBenchmarkTest,      // debug/benchmark_test.h
    kGifCaptureTest,     // debug/gif_capture_test.h
};

// Edit this constant (only) to boot into a diagnostic instead of the chooser menu.
constexpr BootMode kBootMode = BootMode::kNormal;

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

    if constexpr (kBootMode == BootMode::kAtomValidationTest)
    {
        while (true)
        {
            runAtomValidationTest();
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    else if constexpr (kBootMode == BootMode::kColorTest)
    {
        Display display{};
        runColorCalibrationTest(display); // never returns
    }
    else if constexpr (kBootMode == BootMode::kBenchmarkTest)
    {
        Display display{};
        runBenchmarkTest(display);
        ESP_LOGI(kMainTag, "benchmark finished -- capture via `pio device monitor`, watch for BENCH,DONE; "
                           "set kBootMode back to kNormal to return to normal boot");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    else if constexpr (kBootMode == BootMode::kGifCaptureTest)
    {
        Display display{};
        startScreenshotConsole(display); // so pc/pull_screenshots.py can SS_LIST/SS_GET the saved frames afterward
        runGifCaptureTest(display);
        ESP_LOGI(kMainTag, "gif capture finished -- pull frames via `python3 pc/pull_screenshots.py --all`, "
                           "watch for GIF,DONE; set kBootMode back to kNormal to return to normal boot");
        while (true)
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
    else
    {
        Display display{};
        if constexpr (!kIsCYD)
        {
            startScreenshotConsole(display); // 's'/'l'/SS_GET/SS_DEL over the console -- see screenshot_console.h
        }
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

        // After Display (its DMA frame buffers need the internal heap first; a Wi-Fi failure
        // is only logged, the hologram then just runs tilt-only) and before the splash, so the
        // access point is already up by the time the menu appears.
        if constexpr (kWebRemoteEnabled)
        {
            startWebRemote();
            logMemory("startup: web remote");
        }

        display.waitForFlushDone();
        drawSplashScreen(display);
        display.presentFrame();
        vTaskDelay(pdMS_TO_TICKS(kSplashHoldMs));

        if constexpr (kIsCYD)
        {
            // CYD has no IMU (Qmi8658 is a no-op shim, see ux/imu.cpp) -- navigation is driven by
            // the XPT2046 touch panel instead (touch.h/touch_gesture.h). TouchGestureDetector
            // implements the same GestureSource interface as TiltGestureDetector (poll() ->
            // TiltEvent), so runChooser()/runOrbitalView()/runAtomView() are unmodified from the
            // tilt-driven path -- see tilt_gesture.h's GestureSource doc comment. No calibration
            // step: unlike tilt, touch swipes need no learned direction mapping
            // (config/hardware_constants.h's kTouchSwapXY/kTouchInvertDx/kTouchInvertDy handle
            // orientation instead).
            ESP_LOGI(kMainTag, "CYD build: no IMU, using touch panel for navigation");
            Xpt2046 touchPanel{};
            TouchGestureDetector tilt{touchPanel};
            logMemory("startup: chooser");
            runChooser(display, tilt, nullptr); // no IMU on CYD -- steady-state views keep the
                                                 // old synthetic auto-rotation
        }
        else
        {
            Qmi8658 imu{};
            TiltGestureDetector tilt{imu};
            OrientationTracker orientation{imu};
            // Gyro bias/rest-pose calibration cannot be skipped via a hardcoded default like the
            // accelerometer's planar baseline below (see ux/orientation_tracker.h's calibrate()
            // doc comment) -- runs every boot, ~1s, board still resting from the splash hold.
            orientation.calibrate();

            if constexpr (!kTiltNavigationEnabled)
            {
                ESP_LOGI(kMainTag, "tilt navigation disabled (web remote drives selection) -- skipping direction calibration");
                NullGestureSource noGestures{};
                logMemory("startup: chooser");
                runChooser(display, noGestures, &orientation);
            }
            else if (imu.checkPlanarAtBoot())
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
            runChooser(display, tilt, &orientation);
        }
    }
}
