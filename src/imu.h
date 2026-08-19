/**
 * @file imu.h
 * @brief ESP-IDF driver for the QMI8658 6-axis IMU on the Waveshare ESP32-S3-LCD-1.3
 *        (SDA=47, SCL=48).
 *
 * Only the accelerometer is enabled/read -- tilt_gesture.h's detector only needs linear
 * acceleration, not gyro rate. Register map, init sequence, and scale factor (WHO_AM_I=0x6B/
 * 0x05, CTRL1/CTRL2/CTRL7, ACCEL_OUT@0x35, +-4g/8192 LSB-per-g) match
 * boards/QMI8658C_datasheet_rev_0.9.pdf's UI Register Overview table. Built on ESP-IDF's
 * `driver/i2c_master.h` peripheral driver (the standard bus/device handle API) rather than a
 * third-party component -- no dedicated ESP-IDF QMI8658 component exists, and this register
 * sequence is short enough not to need one.
 */
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "orbitals.h" // orb_real_t

class Qmi8658
{
public:
    /**
     * @brief Bring up the I2C bus (SDA=47/SCL=48, 400kHz), probe WHO_AM_I, and configure the
     *        accelerometer (+-4g, 250Hz ODR, accel-only).
     * @note Aborts via ESP_ERROR_CHECK/abort() on failure, matching Display's boot-time error
     *       handling -- this project has no code path that runs meaningfully without the IMU
     *       once this constructor is reached.
     */
    Qmi8658();
    ~Qmi8658();

    Qmi8658(const Qmi8658 &) = delete;
    Qmi8658(Qmi8658 &&) = delete;
    Qmi8658 &operator=(const Qmi8658 &) = delete;
    Qmi8658 &operator=(Qmi8658 &&) = delete;

    /**
     * @brief Read linear acceleration in g, board-local axes (includes gravity -- raw
     *        accelerometer output, not gravity-subtracted).
     * @return false (outputs unwritten) on an I2C transaction failure -- unlike the
     *         constructor's probe, a runtime read glitch shouldn't abort the render loop, so
     *         this is a normal bool return, logged at the caller's discretion
     *         (tilt_gesture.cpp logs it).
     */
    bool readAccelG(orb_real_t *outX, orb_real_t *outY, orb_real_t *outZ);

    /**
     * @brief Quick go/no-go read to decide whether the device is resting in its known-good
     *        boot pose (see tilt_defaults.h), so main.cpp can skip interactive calibration.
     * @note Much faster than TiltGestureDetector::calibrate()'s full ~1s/100-sample average --
     *       this only needs to decide planar-or-not, not produce a trustworthy baseline --
     *       but still averages several samples so a momentary bump right at power-on doesn't
     *       cause a false "not planar". Compares the full 3D reading against tilt_defaults.h's
     *       hardcoded baseline via cosine similarity (much stricter than tilt_gesture.h's
     *       cfg.minDirectionSimilarity, which only disambiguates 4 well-separated gesture
     *       directions) plus a magnitude check (rejects free-fall/being-handled readings whose
     *       direction happens to line up by chance).
     */
    bool checkPlanarAtBoot();

private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;

    bool writeReg(uint8_t reg, uint8_t value);
    bool readRegs(uint8_t reg, uint8_t *buf, size_t len);
};
