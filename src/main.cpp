// Exactly one of the four #define toggles below may be active at a time; with none
// defined, app_main() boots the runtime chooser menu (the real default, see chooser.h) --
// tilt UP launches the orbital viewer, tilt DOWN the element viewer, matching
// micropython/chooser.py's role. ATOM_VIEW is currently active below for direct testing --
// comment it out to reach the chooser instead.

// #define ATOM_VALIDATION_TEST
// #define ATOM_VIEW_TEST
#define ATOM_VIEW
// #define COLOR_TEST

#include <cmath>

#include "chooser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "splash_bitmap.h"
#include "tilt_defaults.h"
#include "tilt_gesture.h"

#ifdef ATOM_VALIDATION_TEST
#include "atom_validation_test.h"
#endif

#include "atom_view_test.h"
#include "atom_view.h"

static const char *kMainTag = "main";

// A short go/no-go read, much faster than TiltGestureDetector::calibrate()'s full ~1s/100-
// sample average (this only needs to decide planar-or-not, not produce a trustworthy
// baseline) -- averaged over kPlanarCheckSamples anyway rather than a single reading, so a
// momentary bump right at power-on doesn't cause a false "not planar".
constexpr int kPlanarCheckSamples = 20;
constexpr uint32_t kPlanarCheckSampleDelayMs = 8;
// Cosine-similarity floor against the hardcoded default baseline -- much stricter than
// tilt_gesture.h's cfg.minDirectionSimilarity (0.7, for disambiguating 4 well-separated
// gesture directions): this must confidently reject "upside-down" or "sideways", not just
// find the closest of a few options. NOT a re-introduction of the single-axis heuristic
// tilt_gesture.h's header comment describes as unreliable -- this compares the full 3D
// vector against a specific pre-calibrated reference, not an assumed sensor axis.
constexpr orb_real_t kPlanarMinSimilarity = orb_real_t(0.995);
// Guards against a magnitude far from 1g (board in free-fall/being handled) reading as
// "planar" just because its direction happens to line up.
constexpr orb_real_t kPlanarMaxMagnitudeDeltaG = orb_real_t(0.15);

// "I need to embed as splash screen a JPG file" (feedback, 2026-08-17) -- shown for a fixed
// hold up front, before the IMU/tilt setup below even starts (see splash_bitmap.h's
// generator docstring for why this is a raw embedded pixel array, not an on-device JPEG
// decode).
constexpr uint32_t kSplashHoldMs = 2000;

static bool checkPlanarAtBoot(Qmi8658 &imu)
{
    orb_real_t sumX = orb_real_t(0), sumY = orb_real_t(0), sumZ = orb_real_t(0);
    int ok = 0;
    for (int i = 0; i < kPlanarCheckSamples; i++)
    {
        orb_real_t x, y, z;
        if (imu.readAccelG(&x, &y, &z))
        {
            sumX += x;
            sumY += y;
            sumZ += z;
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(kPlanarCheckSampleDelayMs));
    }
    if (ok == 0)
    {
        ESP_LOGW(kMainTag, "planar check: no IMU samples read, assuming not planar");
        return false;
    }

    orb_real_t x = sumX / orb_real_t(ok), y = sumY / orb_real_t(ok), z = sumZ / orb_real_t(ok);
    orb_real_t mag = std::sqrt(x * x + y * y + z * z);
    orb_real_t defaultMag = std::sqrt(kDefaultBaselineX * kDefaultBaselineX + kDefaultBaselineY * kDefaultBaselineY +
                                      kDefaultBaselineZ * kDefaultBaselineZ);
    if (mag < orb_real_t(1e-6))
    {
        ESP_LOGW(kMainTag, "planar check: near-zero reading, assuming not planar");
        return false;
    }

    orb_real_t similarity =
        (x * kDefaultBaselineX + y * kDefaultBaselineY + z * kDefaultBaselineZ) / (mag * defaultMag);
    orb_real_t magDelta = mag > defaultMag ? mag - defaultMag : defaultMag - mag;
    bool planar = similarity >= kPlanarMinSimilarity && magDelta <= kPlanarMaxMagnitudeDeltaG;
    ESP_LOGI(kMainTag, "planar check: reading=(%.3f,%.3f,%.3f)g similarity=%.4f magDelta=%.3fg -> %s", double(x),
             double(y), double(z), double(similarity), double(magDelta), planar ? "PLANAR" : "not planar");
    return planar;
}

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

    display.waitForFlushDone();
    drawSplashScreen(display.getFrameBuf());
    display.presentFrame();
    vTaskDelay(pdMS_TO_TICKS(kSplashHoldMs));

    Qmi8658 imu{};
    TiltGestureDetector tilt{imu};

    if (checkPlanarAtBoot(imu))
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
