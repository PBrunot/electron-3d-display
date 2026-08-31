// Consolidated hardware-related constants: the IMU's I2C bus config/register map/accel
// scale, the boot-time planarity-check tuning built on top of it, and the default tilt
// calibration for this specific board's mount angle. NOT the display's SPI pins/clock (those
// stay as #defines in render/display.cpp, tightly coupled to the esp_lcd init code and its
// datasheet-timing rationale comment right next to them) or Display::kDisplayWidth/Height
// (stays a Display static member, used qualified as Display::kDisplayWidth throughout).
#pragma once

#include <cstdint>

#include "physics/orbitals.h" // orb_real_t

// ============================================================================================
// IMU I2C bus (ux/imu.cpp)
// ============================================================================================

inline constexpr uint16_t kImuAddr = 0x6B;
inline constexpr uint32_t kImuI2cFreqHz = 400000;
inline constexpr int kImuXferTimeoutMs = 1000;

// ============================================================================================
// QMI8658 register map (ux/imu.cpp)
// ============================================================================================

inline constexpr uint8_t kRegWhoAmI = 0x00;
inline constexpr uint8_t kRegCtrl1 = 0x02;
inline constexpr uint8_t kRegCtrl2 = 0x03;
inline constexpr uint8_t kRegCtrl7 = 0x08;
inline constexpr uint8_t kRegAccelOut = 0x35; // AX_L, 6 bytes: AX/AY/AZ, little-endian, 2's complement

inline constexpr uint8_t kExpectedWhoAmI = 0x05;

// CTRL2 = accel range in bits [6:4] | ODR in bits [3:0] -- see qmi8658.py's header comment.
inline constexpr uint8_t kRange4gBits = 1;
inline constexpr uint8_t kOdr250HzBits = 5;
inline constexpr orb_real_t kRange4gScale = orb_real_t(8192.0); // LSB/g at +-4g full scale

// ============================================================================================
// Boot-time planarity check tuning (ux/imu.cpp's checkPlanarAtBoot())
// ============================================================================================

// Averaged over kPlanarCheckSamples rather than a single reading, so a momentary bump right
// at power-on doesn't cause a false "not planar".
inline constexpr int kPlanarCheckSamples = 20;
inline constexpr uint32_t kPlanarCheckSampleDelayMs = 8;
// Cosine-similarity floor against the hardcoded default baseline -- see checkPlanarAtBoot()'s
// doc comment (imu.h) for why this must be much stricter than tilt_gesture.h's
// cfg.minDirectionSimilarity.
inline constexpr orb_real_t kPlanarMinSimilarity = orb_real_t(0.995);
// Guards against a magnitude far from 1g (board in free-fall/being handled) reading as
// "planar" just because its direction happens to line up.
inline constexpr orb_real_t kPlanarMaxMagnitudeDeltaG = orb_real_t(0.15);

// ============================================================================================
// Default tilt calibration (this board's known-good resting pose)
// ============================================================================================
//
// Consumed by Qmi8658::checkPlanarAtBoot() (ux/imu.cpp) to skip the interactive
// Right/Left/Up/Down calibration sequence when the device is already resting in its known-good
// pose at boot.
//
// To get real values for a different unit: boot the device once (it'll run full calibration
// if these don't match), then copy the "calibration constants for hardware_constants.h" block
// TiltGestureDetector::logCalibrationForHardcode() prints over serial right after calibration
// completes, and paste it in below, replacing every constant here. Reflash; a normal
// (right-side-up) boot then skips straight to the menu. Powering the device on upside-down
// remains the way to force recalibration afterward (see main.cpp's boot sequence).
inline constexpr orb_real_t kDefaultBaselineX = orb_real_t(-0.0159);
inline constexpr orb_real_t kDefaultBaselineY = orb_real_t(0.3526);
inline constexpr orb_real_t kDefaultBaselineZ = orb_real_t(0.9478);
inline constexpr orb_real_t kDefaultDirRefLeftX = orb_real_t(-0.9857);
inline constexpr orb_real_t kDefaultDirRefLeftY = orb_real_t(-0.0088);
inline constexpr orb_real_t kDefaultDirRefLeftZ = orb_real_t(-0.1683);
inline constexpr orb_real_t kDefaultDirRefRightX = orb_real_t(0.9890);
inline constexpr orb_real_t kDefaultDirRefRightY = orb_real_t(0.0028);
inline constexpr orb_real_t kDefaultDirRefRightZ = orb_real_t(-0.1476);
inline constexpr orb_real_t kDefaultDirRefUpX = orb_real_t(-0.1052);
inline constexpr orb_real_t kDefaultDirRefUpY = orb_real_t(0.9033);
inline constexpr orb_real_t kDefaultDirRefUpZ = orb_real_t(-0.4158);
inline constexpr orb_real_t kDefaultDirRefDownX = orb_real_t(0.0109);
inline constexpr orb_real_t kDefaultDirRefDownY = orb_real_t(-0.9807);
inline constexpr orb_real_t kDefaultDirRefDownZ = orb_real_t(0.1952);

// ============================================================================================
// Touch swipe gesture tuning (ux/touch_gesture.cpp, CYD only -- see ux/touch.h)
// ============================================================================================
//
// Raw XPT2046 ADC units (0..4095, uncalibrated -- see touch.h) the touch point must move from
// its down-point before a direction candidate arms, and the hysteresis floor it must stay above
// to remain armed -- same shape as TiltGestureConfig's thresholdG/releaseG above, just in raw
// touch units since there's no calibrated screen-pixel mapping.
inline constexpr int kTouchSwipeThresholdRaw = 350;
inline constexpr int kTouchSwipeReleaseRaw = 200;
// Sustained-hold duration required to confirm a swipe. Shorter than TiltGestureConfig's default
// holdConfirmMs (1000ms) -- touch swipes felt sluggish at that pace on real hardware.
inline constexpr uint32_t kTouchHoldConfirmMs = 500;

// Orientation of the touch panel's raw X/Y axes relative to the display's on-screen up/down/
// left/right. Verified on real hardware: L/R came out reversed, U/D did not. If this ever needs
// re-tuning, watch the raw dx/dy logged by ux/touch_gesture.cpp (ESP_LOGI, tag "touch_gesture")
// while swiping a known direction, and flip whichever constant makes the logged direction match
// the physical swipe.
inline constexpr bool kTouchSwapXY = false;
inline constexpr bool kTouchInvertDx = true;
inline constexpr bool kTouchInvertDy = false;