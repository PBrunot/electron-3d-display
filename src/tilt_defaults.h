// Hardcoded default calibration constants, consumed by main.cpp's checkPlanarAtBoot() to
// skip the interactive Right/Left/Up/Down calibration sequence when the device is already
// resting in its known-good pose at boot.
//
// PLACEHOLDER VALUES -- these do not match any real hardware yet. Until they're replaced,
// checkPlanarAtBoot() will never match (safe: every boot falls through to full interactive
// calibration, exactly like before this feature existed). To get real values: boot the
// device once (it'll run full calibration since these placeholders won't match), then copy
// the "calibration constants for tilt_defaults.h" block TiltGestureDetector::
// logCalibrationForHardcode() prints over serial right after calibration completes, and
// paste it in below, replacing every constant here. Reflash; a normal (right-side-up) boot
// then skips straight to the menu. Powering the device on upside-down remains the way to
// force recalibration afterward (see main.cpp's boot sequence).
#pragma once

#include "orbitals.h" // orb_real_t

constexpr orb_real_t kDefaultBaselineX = orb_real_t(0.0);
constexpr orb_real_t kDefaultBaselineY = orb_real_t(0.0);
constexpr orb_real_t kDefaultBaselineZ = orb_real_t(1.0);

constexpr orb_real_t kDefaultDirRefLeftX = orb_real_t(-1.0);
constexpr orb_real_t kDefaultDirRefLeftY = orb_real_t(0.0);
constexpr orb_real_t kDefaultDirRefLeftZ = orb_real_t(0.0);

constexpr orb_real_t kDefaultDirRefRightX = orb_real_t(1.0);
constexpr orb_real_t kDefaultDirRefRightY = orb_real_t(0.0);
constexpr orb_real_t kDefaultDirRefRightZ = orb_real_t(0.0);

constexpr orb_real_t kDefaultDirRefUpX = orb_real_t(0.0);
constexpr orb_real_t kDefaultDirRefUpY = orb_real_t(1.0);
constexpr orb_real_t kDefaultDirRefUpZ = orb_real_t(0.0);

constexpr orb_real_t kDefaultDirRefDownX = orb_real_t(0.0);
constexpr orb_real_t kDefaultDirRefDownY = orb_real_t(-1.0);
constexpr orb_real_t kDefaultDirRefDownZ = orb_real_t(0.0);
