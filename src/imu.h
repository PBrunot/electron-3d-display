// ESP-IDF driver for the QMI8658 6-axis IMU on the Waveshare ESP32-S3-LCD-1.3 (CLAUDE.md
// section 2: SDA=47, SCL=48). Only the accelerometer is enabled/read -- tilt_gesture.h's
// detector only needs linear acceleration, not gyro rate, same call micropython/qmi8658.py
// already made for the MicroPython port of this board.
//
// Register map, init sequence, and scale factor are ported unchanged from
// micropython/qmi8658.py (WHO_AM_I=0x6B/0x05, CTRL1/CTRL2/CTRL7, ACCEL_OUT@0x35,
// +-4g/8192 LSB-per-g) -- that driver was verified live on this exact board (WHO_AM_I read
// back 0x05, ~1g-at-rest sanity read), and independently cross-checked here against
// boards/QMI8658C_datasheet_rev_0.9.pdf's UI Register Overview table (same addresses).
// Built on ESP-IDF's `driver/i2c_master.h` peripheral driver (the standard bus/device
// handle API, stable since IDF 5.2 and unchanged in this project's IDF 6.0.1 -- see
// CLAUDE.md section 3's "Nota (2026-08-17)" for the WSL/Windows PlatformIO gotcha that
// version number was confirmed under), not a third-party component -- no dedicated
// ESP-IDF QMI8658 component exists, and this register sequence is short enough not to
// need one.
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "orbitals.h" // orb_real_t

class Qmi8658
{
public:
    /**
     * Bring up the I2C bus (SDA=47/SCL=48, 400kHz -- matches qmi8658.py's freq=400_000),
     * probe WHO_AM_I, and configure the accelerometer (+-4g, 250Hz ODR, accel-only).
     * Aborts via ESP_ERROR_CHECK/abort() on failure, matching Display's boot-time error
     * handling -- this project has no code path that runs meaningfully without the IMU
     * once this constructor is reached.
     */
    Qmi8658();
    ~Qmi8658();

    Qmi8658(const Qmi8658 &) = delete;
    Qmi8658(Qmi8658 &&) = delete;
    Qmi8658 &operator=(const Qmi8658 &) = delete;
    Qmi8658 &operator=(Qmi8658 &&) = delete;

    /**
     * Read linear acceleration in g, board-local axes (includes gravity -- raw
     * accelerometer output, not gravity-subtracted). Returns false (outputs unwritten) on
     * an I2C transaction failure -- unlike the constructor's probe, a runtime read glitch
     * shouldn't abort the render loop, so this is a normal bool return, logged at
     * ESP_LOGW by the caller's discretion (tilt_gesture.cpp logs it).
     */
    bool readAccelG(orb_real_t *outX, orb_real_t *outY, orb_real_t *outZ);

private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;

    bool writeReg(uint8_t reg, uint8_t value);
    bool readRegs(uint8_t reg, uint8_t *buf, size_t len);
};
