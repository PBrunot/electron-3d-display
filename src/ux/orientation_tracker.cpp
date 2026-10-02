#include "ux/orientation_tracker.h"

#include <algorithm>
#include <cmath>

#include "ux/imu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *kOrientTag = "orientation";

// Safety clamp on dt even after a resync(): a single missed resync() call anywhere real time
// passed without update() should not turn into one huge gyro-integration jump.
static constexpr orb_real_t kMaxDtSeconds = orb_real_t(0.1);

// update() calls between raw/fused debug dumps -- ~1s at 60 FPS, frequent enough to follow a
// hand tilt in the serial monitor during axis-sign tuning.
static constexpr int kDebugLogInterval = 60;

static orb_real_t degToRad(orb_real_t deg)
{
    return deg * (kOrbitalPi / orb_real_t(180.0));
}

OrientationTracker::OrientationTracker(Qmi8658 &imu, const OrientationTrackerConfig &cfg) : imu_(imu), cfg_(cfg) {}

void OrientationTracker::calibrate()
{
    orb_real_t sumAx = orb_real_t(0), sumAy = orb_real_t(0), sumAz = orb_real_t(0);
    orb_real_t sumGx = orb_real_t(0), sumGy = orb_real_t(0), sumGz = orb_real_t(0);
    orb_real_t sumGyroMag = orb_real_t(0), sumGyroMagSq = orb_real_t(0);
    int ok = 0;
    for (int i = 0; i < cfg_.gyroCalibrationSamples; i++)
    {
        orb_real_t ax, ay, az, gx, gy, gz;
        if (imu_.readAccelGyro(&ax, &ay, &az, &gx, &gy, &gz))
        {
            sumAx += ax;
            sumAy += ay;
            sumAz += az;
            sumGx += gx;
            sumGy += gy;
            sumGz += gz;
            orb_real_t gyroMag = std::sqrt(gx * gx + gy * gy + gz * gz);
            sumGyroMag += gyroMag;
            sumGyroMagSq += gyroMag * gyroMag;
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(cfg_.gyroCalibrationSampleDelayMs));
    }

    fusedPitch_ = orb_real_t(0);
    fusedRoll_ = orb_real_t(0);
    fusedYaw_ = orb_real_t(0);
    lastUpdateUs_ = 0;
    lastMotionUs_ = esp_timer_get_time();
    spin_ = CameraState{0, 0, 0};

    if (ok == 0)
    {
        ESP_LOGW(kOrientTag, "calibrate: no IMU samples read, keeping previous bias/offset");
        return;
    }

    gyroBiasX_ = sumGx / orb_real_t(ok);
    gyroBiasY_ = sumGy / orb_real_t(ok);
    gyroBiasZ_ = sumGz / orb_real_t(ok);

    orb_real_t meanAx = sumAx / orb_real_t(ok), meanAy = sumAy / orb_real_t(ok), meanAz = sumAz / orb_real_t(ok);
    pitchOffset_ = std::atan2(-meanAx, std::sqrt(meanAy * meanAy + meanAz * meanAz));
    rollOffset_ = std::atan2(meanAy, meanAz);

    orb_real_t stdDev = orb_real_t(0);
    if (ok > 1)
    {
        orb_real_t meanMag = sumGyroMag / orb_real_t(ok);
        orb_real_t variance = sumGyroMagSq / orb_real_t(ok) - meanMag * meanMag;
        stdDev = variance > orb_real_t(0) ? std::sqrt(variance) : orb_real_t(0);
    }
    ESP_LOGI(kOrientTag,
             "calibrated (%d/%d samples ok): gyroBias=(%.3f,%.3f,%.3f)dps pitchOffset=%.3frad rollOffset=%.3frad "
             "gyroStdDev=%.3fdps",
             ok, cfg_.gyroCalibrationSamples, double(gyroBiasX_), double(gyroBiasY_), double(gyroBiasZ_),
             double(pitchOffset_), double(rollOffset_), double(stdDev));
    if (stdDev > cfg_.calibrationMaxGyroStdDevDps)
        ESP_LOGW(kOrientTag, "gyro stddev %.3fdps exceeds %.3fdps -- board may not have been held still",
                 double(stdDev), double(cfg_.calibrationMaxGyroStdDevDps));
}

void OrientationTracker::resync() { lastUpdateUs_ = esp_timer_get_time(); }

void OrientationTracker::update()
{
    orb_real_t ax, ay, az, gx, gy, gz;
    if (!imu_.readAccelGyro(&ax, &ay, &az, &gx, &gy, &gz))
        return; // read glitch -- keep the last fused values, same tolerance as tilt_gesture.cpp

    int64_t now = esp_timer_get_time();
    orb_real_t dt = lastUpdateUs_ == 0 ? orb_real_t(0) : orb_real_t(double(now - lastUpdateUs_) / 1e6);
    lastUpdateUs_ = now;
    dt = std::clamp(dt, orb_real_t(0), kMaxDtSeconds);

    orb_real_t gxc = gx - gyroBiasX_, gyc = gy - gyroBiasY_, gzc = gz - gyroBiasZ_;
    if (std::sqrt(gxc * gxc + gyc * gyc + gzc * gzc) > cfg_.motionThresholdDps)
        lastMotionUs_ = now;

    orb_real_t pitchSign = orb_real_t(cfg_.pitchAxisSign);
    orb_real_t rollSign = orb_real_t(cfg_.rollAxisSign);
    orb_real_t yawSign = orb_real_t(cfg_.yawAxisSign);

    orb_real_t gyroPitchRad = degToRad(gx - gyroBiasX_) * pitchSign;
    orb_real_t gyroRollRad = degToRad(gy - gyroBiasY_) * rollSign;
    orb_real_t gyroYawRad = degToRad(gz - gyroBiasZ_) * yawSign;

    orb_real_t pitchAccel = (std::atan2(-ax, std::sqrt(ay * ay + az * az)) - pitchOffset_) * pitchSign;
    orb_real_t rollAccel = (std::atan2(ay, az) - rollOffset_) * rollSign;

    orb_real_t alpha = cfg_.complementaryAlpha;
    fusedPitch_ = alpha * (fusedPitch_ + gyroPitchRad * dt) + (orb_real_t(1) - alpha) * pitchAccel;
    fusedRoll_ = alpha * (fusedRoll_ + gyroRollRad * dt) + (orb_real_t(1) - alpha) * rollAccel;

    // Yaw has no accelerometer correction (no magnetometer on this IMU) -- pure gyro
    // integration, with an optional slow pull back toward zero to bound its drift while
    // stationary (see orientation_tracker.h's header comment for the trade-off).
    fusedYaw_ += gyroYawRad * dt;
    if (cfg_.yawRecenterPerSecond > orb_real_t(0))
        fusedYaw_ *= (orb_real_t(1) - std::min(cfg_.yawRecenterPerSecond * dt, orb_real_t(1)));

    // Periodic raw+fused dump for the on-hardware axis-sign tuning pass (see
    // orientation_tracker.h's OrientationTrackerConfig::{pitch,roll,yaw}AxisSign comment): tilt
    // the board around one axis at a time, watch which fused angle moves and which way, flip
    // the sign that doesn't match.
    if (++debugLogCounter_ >= kDebugLogInterval)
    {
        debugLogCounter_ = 0;
        ESP_LOGI(kOrientTag, "raw accel=(%.2f,%.2f,%.2f)g gyro=(%.1f,%.1f,%.1f)dps -> fused pitch=%.2f roll=%.2f yaw=%.2f rad",
                 double(ax), double(ay), double(az), double(gx), double(gy), double(gz), double(fusedPitch_),
                 double(fusedRoll_), double(fusedYaw_));
    }
}

void OrientationTracker::step(CameraState *cam)
{
    update();
    if (esp_timer_get_time() - lastMotionUs_ > cfg_.idleSpinDelayUs)
    {
        auto advance = [](orb_real_t a, orb_real_t step)
        {
            a += step;
            return a >= kTwoPi ? a - kTwoPi : a;
        };
        spin_.yaw = advance(spin_.yaw, kCameraAngleStep);
        spin_.tilt = advance(spin_.tilt, kCameraTiltStep);
        spin_.roll = advance(spin_.roll, kCameraRollStep);
    }
    cam->yaw = yawRad() + spin_.yaw;
    cam->tilt = kCameraTiltStart + tiltRad() + spin_.tilt;
    cam->roll = kCameraRollStart + rollRad() + spin_.roll;
}

orb_real_t OrientationTracker::tiltRad() const
{
    return std::clamp(fusedPitch_ * cfg_.rotationGain, -cfg_.tiltClampRad, cfg_.tiltClampRad);
}

orb_real_t OrientationTracker::rollRad() const
{
    return std::clamp(fusedRoll_ * cfg_.rotationGain, -cfg_.rollClampRad, cfg_.rollClampRad);
}
