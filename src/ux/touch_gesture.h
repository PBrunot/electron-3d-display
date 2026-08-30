/**
 * @file touch_gesture.h
 * @brief Swipe-and-hold gesture detector for the CYD's XPT2046 touch panel (touch.h),
 *        producing the same TiltEvent vocabulary as tilt_gesture.h's TiltGestureDetector.
 *
 * Every view/chooser call site only ever calls poll() (see tilt_gesture.h's GestureSource), so
 * this class is a drop-in replacement wherever TiltGestureDetector is used today, for a board
 * with a touch panel instead of an IMU.
 *
 * Same interaction model as tilt, just in touch-drag terms instead of accelerometer-deviation
 * terms: press down, drag past kTouchSwipeThresholdRaw (config/hardware_constants.h) in a
 * direction, then HOLD there (finger still down, still past the release threshold) for
 * kTouchHoldConfirmMs to confirm -- an edge-triggered kConfirmed, exactly like
 * TiltGestureDetector's tilt-and-hold, so the existing progress-arrow rendering
 * (drawTiltArrow[At]()) and idle-activity handling in chooser.cpp/orbital_view.cpp/atom_view.cpp
 * all work unchanged. Lifting the finger, or dragging back inside kTouchSwipeReleaseRaw,
 * cancels the candidate with no action taken.
 *
 * No direction-mapping calibration step (unlike tilt): the touch panel's raw X/Y axes are fixed
 * at compile time via config/hardware_constants.h's kTouchSwapXY/kTouchInvertDx/kTouchInvertDy
 * -- see that header's comment for how to tune them against real hardware.
 */
#pragma once

#include <cstdint>

#include "ux/tilt_gesture.h" // TiltEvent/TiltDirection/TiltPhase/GestureSource
#include "ux/touch.h"

class TouchGestureDetector : public GestureSource
{
public:
    explicit TouchGestureDetector(Xpt2046 &touch);

    /// Read the touch panel once and advance the hold state machine -- call once per
    /// frame/tick, same contract as TiltGestureDetector::poll().
    TiltEvent poll() override;

private:
    Xpt2046 &touch_;

    bool down_ = false;   ///< Finger currently on the panel.
    bool active_ = false; ///< A direction candidate is armed (implies down_).
    uint16_t downX_ = 0, downY_ = 0; ///< Raw touch-down anchor point.
    TiltDirection activeDir_ = TiltDirection::kNone;
    uint32_t holdStartMs_ = 0;
    bool confirmedFired_ = false; // latched once kConfirmed has fired for this hold, so a
                                   // still-held swipe returns kHolding (not kConfirmed again)
                                   // on every subsequent poll() until release
};
