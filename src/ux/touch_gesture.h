/**
 * @file touch_gesture.h
 * @brief Swipe-and-hold gesture detector for a polled touch panel (touch_panel.h), producing
 *        the same TiltEvent vocabulary as tilt_gesture.h's TiltGestureDetector.
 *
 * Every view/chooser call site only ever calls poll() (see tilt_gesture.h's GestureSource), so
 * this class is a drop-in replacement wherever TiltGestureDetector is used today, for a board
 * with a touch panel instead of an IMU. Works against any TouchPanel implementation -- the CYD's
 * bit-banged XPT2046 (touch.h, raw uncalibrated ADC units) and the ES3C28P's I2C FT6336G
 * (ft6336g.h, calibrated screen-pixel coordinates) -- since TouchGestureConfig's
 * swipeThreshold/swipeRelease are supplied per-panel in that panel's own native unit.
 *
 * Same interaction model as tilt, just in touch-drag terms instead of accelerometer-deviation
 * terms: press down, drag past cfg.swipeThreshold in a direction, then HOLD there (finger still
 * down, still past cfg.swipeRelease) for cfg.holdConfirmMs to confirm -- an edge-triggered
 * kConfirmed, exactly like TiltGestureDetector's tilt-and-hold, so the existing progress-arrow
 * rendering (drawTiltArrow[At]()) and idle-activity handling in
 * chooser.cpp/orbital_view.cpp/atom_view.cpp all work unchanged. Lifting the finger, or dragging
 * back inside cfg.swipeRelease, cancels the candidate with no action taken.
 *
 * No direction-mapping calibration step (unlike tilt): the touch panel's raw X/Y axes are fixed
 * at construction time via TouchGestureConfig's swapXY/invertDx/invertDy (defaults come from
 * config/hardware_constants.h's kTouchSwapXY/kTouchInvertDx/kTouchInvertDy, the CYD's verified
 * values) -- see that header's comment for how to tune them against real hardware.
 */
#pragma once

#include <cstdint>

#include "ux/tilt_gesture.h" // TiltEvent/TiltDirection/TiltPhase/GestureSource
#include "ux/touch_panel.h"
#include "config/hardware_constants.h" // kTouchSwipeThresholdRaw/ReleaseRaw, kTouchHoldConfirmMs, kTouchSwapXY/InvertDx/InvertDy

struct TouchGestureConfig
{
    /// Movement (in the touch panel's native unit) from the touch-down anchor point needed to
    /// arm a direction candidate. Default matches the CYD's XPT2046 (raw ADC units); pass an
    /// ES3C28P-tuned TouchGestureConfig (config/hardware_constants.h's kCapTouchSwipeThresholdPx,
    /// in screen pixels) when driving an Ft6336g instead.
    int swipeThreshold = kTouchSwipeThresholdRaw;
    /// Movement must drop below this to release an armed candidate -- hysteresis, same role as
    /// TiltGestureConfig::releaseG.
    int swipeRelease = kTouchSwipeReleaseRaw;
    /// Sustained-hold duration required to confirm a swipe.
    uint32_t holdConfirmMs = kTouchHoldConfirmMs;

    bool swapXY = kTouchSwapXY;
    bool invertDx = kTouchInvertDx;
    bool invertDy = kTouchInvertDy;
};

class TouchGestureDetector : public GestureSource
{
public:
    explicit TouchGestureDetector(TouchPanel &touch, const TouchGestureConfig &cfg = TouchGestureConfig());

    /// Read the touch panel once and advance the hold state machine -- call once per
    /// frame/tick, same contract as TiltGestureDetector::poll().
    TiltEvent poll() override;

private:
    TouchPanel &touch_;
    TouchGestureConfig cfg_;

    bool down_ = false;   ///< Finger currently on the panel.
    bool active_ = false; ///< A direction candidate is armed (implies down_).
    uint16_t downX_ = 0, downY_ = 0; ///< Raw touch-down anchor point.
    TiltDirection activeDir_ = TiltDirection::kNone;
    uint32_t holdStartMs_ = 0;
    bool confirmedFired_ = false; // latched once kConfirmed has fired for this hold, so a
                                   // still-held swipe returns kHolding (not kConfirmed again)
                                   // on every subsequent poll() until release
};
