/**
 * @file touch_panel.h
 * @brief Common interface for a polled touch panel, abstracting over different touch
 *        hardware/protocols so ux/touch_gesture.h's swipe-and-hold detector works unmodified
 *        against any of them.
 *
 * Two implementations exist: ux/touch.h's Xpt2046 (CYD, bit-banged SPI, raw uncalibrated 12-bit
 * ADC units) and ux/ft6336g.h's Ft6336g (ES3C28P, I2C, calibrated screen-pixel coordinates).
 * "Raw" in readRaw() just means "whatever native unit this hardware reports" -- touch_gesture.h
 * only needs consistent relative movement between samples, not absolute on-screen coordinates,
 * so it doesn't care whether that unit is ADC counts or pixels (see TouchGestureConfig's
 * swipeThreshold/swipeRelease, which are tuned per-panel accordingly).
 */
#pragma once

#include <cstdint>

class TouchPanel
{
public:
    virtual ~TouchPanel() = default;

    /// True while the panel is currently pressed.
    virtual bool isTouched() const = 0;

    /**
     * @brief One touch-point sample, in this panel's native unit.
     * @return false (outputs unwritten) if the panel isn't currently touched.
     */
    virtual bool readRaw(uint16_t *outX, uint16_t *outY) = 0;
};
