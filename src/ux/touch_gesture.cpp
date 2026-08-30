#include "ux/touch_gesture.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "config/hardware_constants.h" // kTouchSwipeThresholdRaw/ReleaseRaw, kTouchHoldConfirmMs, kTouchSwapXY/InvertDx/InvertDy

static const char *kTouchGestureTag = "touch_gesture";

static uint32_t nowMs()
{
    return uint32_t(esp_timer_get_time() / 1000);
}

TouchGestureDetector::TouchGestureDetector(Xpt2046 &touch) : touch_(touch) {}

TiltEvent TouchGestureDetector::poll()
{
    uint16_t x = 0, y = 0;
    bool touched = touch_.readRaw(&x, &y);
    uint32_t now = nowMs();

    if (!touched)
    {
        if (active_)
            ESP_LOGI(kTouchGestureTag, "released %s after %ums", tiltDirectionName(activeDir_), now - holdStartMs_);
        down_ = false;
        active_ = false;
        return TiltEvent{};
    }

    if (!down_)
    {
        // Fresh touch-down: remember the anchor point every later sample in this stroke is
        // measured against. No direction yet -- that needs movement past the threshold below.
        down_ = true;
        downX_ = x;
        downY_ = y;
        active_ = false;
        return TiltEvent{};
    }

    int rawDx = int(x) - int(downX_);
    int rawDy = int(y) - int(downY_);
    // Map the touch panel's raw axes onto screen left/right/up/down -- see
    // hardware_constants.h's kTouchSwapXY/kTouchInvertDx/kTouchInvertDy doc comment.
    int dx = kTouchSwapXY ? rawDy : rawDx;
    int dy = kTouchSwapXY ? rawDx : rawDy;
    if (kTouchInvertDx)
        dx = -dx;
    if (kTouchInvertDy)
        dy = -dy;

    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int mag = adx > ady ? adx : ady; // dominant-axis magnitude, matching the direction pick below

    if (!active_)
    {
        if (mag < kTouchSwipeThresholdRaw)
            return TiltEvent{};

        // Latch the dominant axis/sign for the whole hold, same as TiltGestureDetector latching
        // its deviation direction -- a multi-second hold on a resistive panel won't stay
        // perfectly steady either.
        activeDir_ = adx > ady ? (dx > 0 ? TiltDirection::kRight : TiltDirection::kLeft)
                                : (dy > 0 ? TiltDirection::kDown : TiltDirection::kUp);
        ESP_LOGI(kTouchGestureTag, "candidate: %s (dx=%d dy=%d)", tiltDirectionName(activeDir_), dx, dy);

        active_ = true;
        holdStartMs_ = now;
        confirmedFired_ = false;
        return TiltEvent{activeDir_, TiltPhase::kHolding, 0};
    }

    if (mag < kTouchSwipeReleaseRaw)
    {
        ESP_LOGI(kTouchGestureTag, "candidate %s dropped below release threshold (dx=%d dy=%d)",
                 tiltDirectionName(activeDir_), dx, dy);
        active_ = false;
        return TiltEvent{};
    }

    uint32_t heldMs = now - holdStartMs_;
    if (!confirmedFired_ && heldMs >= kTouchHoldConfirmMs)
    {
        confirmedFired_ = true;
        ESP_LOGI(kTouchGestureTag, "CONFIRMED %s after %ums", tiltDirectionName(activeDir_), heldMs);
        return TiltEvent{activeDir_, TiltPhase::kConfirmed, heldMs};
    }
    return TiltEvent{activeDir_, TiltPhase::kHolding, heldMs};
}
