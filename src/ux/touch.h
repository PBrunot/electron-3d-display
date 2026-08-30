/**
 * @file touch.h
 * @brief ESP-IDF bit-banged driver for the XPT2046 resistive touch controller on the CYD
 *        (CLK=25, MOSI/DIN=32, CS=33, IRQ=36, MISO/DOUT=39 -- see CYD-branch.md's pin table).
 *
 * Bit-banged, not the hardware SPI peripheral driver: both ESP32 general-purpose SPI buses are
 * already claimed (HSPI drives the ILI9341 display, VSPI the SD card -- render/display.cpp,
 * CYD-branch.md) and classic ESP32 has no third hardware SPI controller available for
 * peripherals. The read rate needed here (gesture polling once per frame, not a real touch UI)
 * is low enough that a bit-banged clock well under the chip's ~2MHz limit costs nothing worth
 * avoiding a third bus for.
 *
 * Only what touch_gesture.h's swipe detector needs: touched-or-not (via the IRQ pin, which the
 * chip pulls low while the panel is pressed -- the standard touch-detect signal, simpler and
 * more reliable here than thresholding the Z1/Z2 pressure channels) and raw 12-bit X/Y ADC
 * samples. No screen-pixel calibration step: touch_gesture.h only needs consistent relative
 * movement between samples, not absolute on-screen coordinates, so unlike imu.h's
 * checkPlanarAtBoot() there is nothing here to calibrate against a known-good reading.
 *
 * @note No-op on non-CYD targets, mirroring imu.h's Qmi8658 CYD no-op: the Waveshare S3 board
 *       has no XPT2046 and never constructs this class.
 * @note The XPT2046 has no identifying register (unlike the QMI8658's WHO_AM_I), so there is no
 *       probe/handshake to abort boot over -- a miswired or absent panel just never reports
 *       isTouched(), same failure mode as any other disconnected GPIO.
 */
#pragma once

#include <cstdint>

class Xpt2046
{
public:
    /// Configures CLK/MOSI/CS as outputs and MISO/IRQ as inputs (IRQ with an internal pull-up,
    /// since the panel only pulls it low while touched).
    Xpt2046();

    /// True while the panel is currently pressed (IRQ pin low).
    bool isTouched() const;

    /**
     * @brief Raw 12-bit ADC samples (0..4095, uncalibrated -- see file comment).
     * @return false (outputs unwritten) if the panel isn't currently touched, or if the touch
     *         was released mid-transaction (resistive panels bounce at the edge of contact --
     *         a torn sample here is discarded rather than returned as a false direction).
     */
    bool readRaw(uint16_t *outX, uint16_t *outY);

private:
    /// One 8-bit-command/16-bit-response bit-banged transaction; returns the 12-bit result
    /// (already shifted down out of the response's 3 trailing zero-pad bits).
    uint16_t xferCmd(uint8_t cmd) const;
};
