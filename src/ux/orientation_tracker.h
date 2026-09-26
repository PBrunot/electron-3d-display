/**
 * @file orientation_tracker.h
 * @brief Accelerometer+gyroscope complementary-filter fusion driving the steady-state tumble
 *        camera (render/camera.h's CameraState), so tilting the physical board rotates the
 *        rendered atom/orbital like a real object held in the hand -- absolute orientation
 *        mapping, not rate/velocity control (see plans/bright-plotting-boole.md's rationale).
 *
 * Pitch and roll come from the accelerometer's gravity vector (atan2, absolute, no drift) with
 * gyroscope integration blended in between accelerometer samples for smoothness (a standard
 * complementary filter, see cfg.complementaryAlpha). Yaw has no absolute reference -- the
 * QMI8658 has no magnetometer -- so it's pure gyroscope integration and WILL drift; a slow
 * exponential pull toward zero (cfg.yawRecenterPerSecond) bounds that drift while stationary, at
 * the cost of also relaxing a deliberately-held yaw over tens of seconds. This is a deliberate
 * trade-off for a handheld toy, not a navigation-grade AHRS.
 *
 * Not a replacement for tilt_gesture.h's TiltGestureDetector: that stays the discrete
 * extreme-tilt navigation gesture (own accelerometer read, own baseline/thresholds). This class
 * only drives the continuous "look at it from this angle" rotation in atom_view.cpp's/
 * orbital_view.cpp's steady-state loop -- see their stepCamera() call sites.
 */
#pragma once

#include <cstdint>

class Qmi8658;

#include "physics/orbitals.h" // orb_real_t

struct OrientationTrackerConfig
{
    /// Weight given to the gyro-integrated term in the complementary filter (pitch/roll); the
    /// remainder (1-alpha) comes from the accelerometer's absolute (drift-free) angle each
    /// update(). Yaw has no accelerometer term, so this doesn't apply to it.
    orb_real_t complementaryAlpha = orb_real_t(0.98);

    /// Symmetric clamp (radians) on the reported tilt/roll -- physical tilt has a natural
    /// limited range, unlike render/camera.h's stepCamera() which wraps through the full circle.
    /// Must stay comfortably below tilt_gesture.h's TiltGestureConfig::thresholdG's *effective*
    /// deviation so continuous rotation saturates before the discrete extreme-tilt navigation
    /// gesture fires -- tune both together on hardware (see the plan's verification steps).
    orb_real_t tiltClampRad = orb_real_t(0.61); // ~35 degrees
    orb_real_t rollClampRad = orb_real_t(0.61);

    /// Fraction of the accumulated yaw pulled back toward zero per second of real time (0
    /// disables the pull entirely). Bounds yaw's unavoidable gyro-only drift while the device
    /// sits still, at the cost of also relaxing a deliberately-held yaw over time -- the first
    /// knob to turn down (even to 0) if that relaxation is more noticeable than the drift it's
    /// meant to hide.
    orb_real_t yawRecenterPerSecond = orb_real_t(0.05);

    /// Same shape as TiltGestureConfig::calibrationSamples/calibrationSampleDelayMs -- long/slow
    /// enough (samples * delayMs =~ 1s) for a stable gyro-bias and rest-pose reading. Unlike the
    /// accelerometer's "planar" baseline (which can reuse a hardcoded default across boots, see
    /// Qmi8658::checkPlanarAtBoot()), gyro bias depends on temperature/time-since-reset and must
    /// be recaptured live every boot.
    int gyroCalibrationSamples = 100;
    uint32_t gyroCalibrationSampleDelayMs = 10;
    /// If the gyro magnitude samples vary by more than this during calibrate(), log a warning --
    /// same rationale as TiltGestureConfig::calibrationMaxStdDevG.
    orb_real_t calibrationMaxGyroStdDevDps = orb_real_t(1.5);

    /// Per-axis sign flip applied to both the gyro rate and the accelerometer-derived angle
    /// feeding pitch/roll/yaw -- there is no known mapping between the IMU's physical axes and
    /// which way the rendered atom should turn, so this starts at +1 and gets tuned on real
    /// hardware (tilt one axis at a time, watch which fused angle moves and which way, flip the
    /// sign that doesn't match). See the plan's "Tuning assi" step.
    int pitchAxisSign = 1;
    int rollAxisSign = 1;
    int yawAxisSign = 1;
};

/// Fuses Qmi8658 accelerometer+gyroscope readings into a continuous absolute pitch/roll/yaw
/// estimate. One instance shared across the app lifetime (constructed next to the shared
/// Qmi8658/TiltGestureDetector in main.cpp), threaded down to runChooser()/runAtomView()/
/// runOrbitalView() as a nullable pointer (nullptr on CYD, which has no IMU).
class OrientationTracker
{
public:
    explicit OrientationTracker(Qmi8658 &imu, const OrientationTrackerConfig &cfg = OrientationTrackerConfig());

    /// Average cfg.gyroCalibrationSamples accel+gyro readings (~1s, board resting still) into
    /// the gyro bias and the accelerometer rest-pose pitch/roll offset, and reset the fused
    /// state to zero. Call once at boot, right after construction, before checkPlanarAtBoot()'s
    /// branch (same "still resting after the splash hold" assumption) -- unlike the
    /// accelerometer's planar baseline, this cannot be skipped via a hardcoded default.
    void calibrate();

    /// Reset the internal dt clock without touching the fused angles -- call after any stretch
    /// of real wall-clock time that passed without a normal update() (fly-overs, dissection,
    /// idle jumps, zoom excursions: anywhere atom_view.cpp/orbital_view.cpp already call
    /// FrameStats::reset() for the same reason), so the next update() doesn't see a multi-second
    /// dt and slam the gyro integrator.
    void resync();

    /// Read the IMU once, fuse, and integrate -- call once per steady-state frame. A read
    /// failure leaves the fused state unchanged (same tolerance as TiltGestureDetector::poll()).
    void update();

    orb_real_t yawRad() const { return fusedYaw_; }
    /// Clamped to +-cfg.tiltClampRad -- not wrapped, unlike render/camera.h's stepCamera().
    orb_real_t tiltRad() const;
    /// Clamped to +-cfg.rollClampRad -- not wrapped, unlike render/camera.h's stepCamera().
    orb_real_t rollRad() const;

private:
    Qmi8658 &imu_;
    OrientationTrackerConfig cfg_;

    orb_real_t gyroBiasX_ = orb_real_t(0), gyroBiasY_ = orb_real_t(0), gyroBiasZ_ = orb_real_t(0);
    orb_real_t pitchOffset_ = orb_real_t(0), rollOffset_ = orb_real_t(0);
    orb_real_t fusedPitch_ = orb_real_t(0), fusedRoll_ = orb_real_t(0), fusedYaw_ = orb_real_t(0);
    int64_t lastUpdateUs_ = 0; // 0 means "no prior sample" -- update()'s first call after
                               // calibrate()/resync() contributes no gyro integration (dt=0).
    int debugLogCounter_ = 0; // throttles update()'s periodic raw/fused dump, see kFpsUpdateInterval
};
