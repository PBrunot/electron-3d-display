/**
 * @file ft6336g.h
 * @brief ESP-IDF I2C driver for the FT6336G capacitive touch controller on the ES3C28P board
 *        (SDA=16, SCL=15, RST=18 -- see ES3C28P_ES2N28P_Specification_V1.0.pdf section 4.2).
 *
 * Polled over I2C (address 0x38), same "once per frame" cadence as ux/touch.h's Xpt2046 -- the
 * INT pin (GPIO17) is not used: the TD_STATUS register already reports touched-or-not directly,
 * so there is nothing an interrupt would add here. Unlike the XPT2046's raw uncalibrated ADC
 * output, the FT6336G reports already-calibrated screen-pixel coordinates (0..239, 0..319).
 *
 * Register map (DEVICE_MODE/TD_STATUS/TOUCH1_X/TOUCH1_Y) is FocalTech's standard FT6x36-family
 * layout, shared across FT6206/FT6236/FT6336 and used consistently by third-party drivers (e.g.
 * Adafruit_FT6206, LVGL's esp_lcd_touch_ft6336) -- this repo has no FT6336G-specific datasheet
 * to confirm a CHIP_ID constant against, so unlike imu.h's Qmi8658 WHO_AM_I probe, there is no
 * probe/abort here; a miswired or absent panel just never reports isTouched().
 *
 * @note No-op on non-ES3C28P targets, mirroring ux/touch.h's Xpt2046 CYD no-op: neither the
 *       Waveshare S3 nor the CYD has this chip and neither ever constructs this class.
 */
#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "ux/touch_panel.h"

class Ft6336g : public TouchPanel
{
public:
    /// Pulses RST low then high, then brings up the I2C bus (SDA=16/SCL=15, 100kHz).
    Ft6336g();
    ~Ft6336g();

    Ft6336g(const Ft6336g &) = delete;
    Ft6336g(Ft6336g &&) = delete;
    Ft6336g &operator=(const Ft6336g &) = delete;
    Ft6336g &operator=(Ft6336g &&) = delete;

    /// True while the panel is currently pressed (TD_STATUS != 0).
    bool isTouched() const override;

    /**
     * @brief Calibrated screen-pixel coordinates of touch point 1 (0..239, 0..319).
     * @return false (outputs unwritten) if the panel isn't currently touched.
     */
    bool readRaw(uint16_t *outX, uint16_t *outY) override;

private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;

    /// Reads TD_STATUS + TOUCH1_XH/XL/YH/YL (registers 0x02..0x06) in one transaction.
    bool readTouch1(uint16_t *outX, uint16_t *outY) const;
};
