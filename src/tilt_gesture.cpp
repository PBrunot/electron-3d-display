#include "tilt_gesture.h"

#include <algorithm>
#include <cmath>

#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *kTiltTag = "tilt";

const char *tiltDirectionName(TiltDirection dir)
{
    switch (dir)
    {
    case TiltDirection::kLeft:
        return "L";
    case TiltDirection::kRight:
        return "R";
    case TiltDirection::kUp:
        return "U";
    case TiltDirection::kDown:
        return "D";
    default:
        return "-";
    }
}

static uint32_t nowMs()
{
    return uint32_t(esp_timer_get_time() / 1000);
}

TiltGestureDetector::TiltGestureDetector(Qmi8658 &imu, const TiltGestureConfig &cfg) : imu_(imu), cfg_(cfg) {}

void TiltGestureDetector::calibrate()
{
    orb_real_t sumX = orb_real_t(0), sumY = orb_real_t(0), sumZ = orb_real_t(0);
    int ok = 0;
    for (int i = 0; i < cfg_.calibrationSamples; i++)
    {
        orb_real_t x, y, z;
        if (imu_.readAccelG(&x, &y, &z))
        {
            sumX += x;
            sumY += y;
            sumZ += z;
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(cfg_.calibrationSampleDelayMs));
    }
    if (ok > 0)
    {
        baseX_ = sumX / orb_real_t(ok);
        baseY_ = sumY / orb_real_t(ok);
        baseZ_ = sumZ / orb_real_t(ok);
    }
    ESP_LOGI(kTiltTag, "calibrated baseline (%d/%d samples ok): x=%.3fg y=%.3fg z=%.3fg", ok,
             cfg_.calibrationSamples, double(baseX_), double(baseY_), double(baseZ_));
}

void TiltGestureDetector::setMapping(int axis, int sign, TiltDirection dir)
{
    if (axis < 0 || axis > 2)
        return;
    axisSignMap_[axis][sign > 0 ? 1 : 0] = dir;
    ESP_LOGI(kTiltTag, "mapping set: axis=%d sign=%+d -> %s", axis, sign, tiltDirectionName(dir));
}

RawTiltEvent TiltGestureDetector::pollRaw()
{
    orb_real_t x, y, z;
    if (!imu_.readAccelG(&x, &y, &z))
    {
        ESP_LOGW(kTiltTag, "IMU read failed, skipping this poll");
        if (activeAxis_ < 0)
            return RawTiltEvent{};
        return RawTiltEvent{activeAxis_, activeSign_, TiltPhase::kHolding, nowMs() - holdStartMs_};
    }

    orb_real_t dx = x - baseX_, dy = y - baseY_, dz = z - baseZ_;
    orb_real_t mag = std::sqrt(dx * dx + dy * dy + dz * dz);
    uint32_t now = nowMs();

    if (activeAxis_ < 0)
    {
        if (mag < cfg_.thresholdG)
            return RawTiltEvent{};

        orb_real_t devs[3] = {dx, dy, dz};
        int axis = 0;
        if (std::abs(double(devs[1])) > std::abs(double(devs[axis])))
            axis = 1;
        if (std::abs(double(devs[2])) > std::abs(double(devs[axis])))
            axis = 2;
        int sign = devs[axis] > orb_real_t(0) ? +1 : -1;

        ESP_LOGI(kTiltTag, "candidate: axis=%d sign=%+d mag=%.2fg", axis, sign, double(mag));

        activeAxis_ = axis;
        activeSign_ = sign;
        holdStartMs_ = now;
        confirmedFired_ = false;
        return RawTiltEvent{axis, sign, TiltPhase::kHolding, 0};
    }

    if (mag < cfg_.releaseG)
    {
        ESP_LOGI(kTiltTag, "released axis=%d sign=%+d after %ums (mag=%.2fg)", activeAxis_, activeSign_,
                 now - holdStartMs_, double(mag));
        activeAxis_ = -1;
        activeSign_ = 0;
        return RawTiltEvent{};
    }

    uint32_t heldMs = now - holdStartMs_;
    if (!confirmedFired_ && heldMs >= cfg_.holdConfirmMs)
    {
        confirmedFired_ = true;
        ESP_LOGI(kTiltTag, "CONFIRMED axis=%d sign=%+d after %ums", activeAxis_, activeSign_, heldMs);
        return RawTiltEvent{activeAxis_, activeSign_, TiltPhase::kConfirmed, heldMs};
    }
    return RawTiltEvent{activeAxis_, activeSign_, TiltPhase::kHolding, heldMs};
}

TiltEvent TiltGestureDetector::poll()
{
    RawTiltEvent raw = pollRaw();
    if (raw.axis < 0)
        return TiltEvent{};

    TiltDirection dir = axisSignMap_[raw.axis][raw.sign > 0 ? 1 : 0];
    if (dir == TiltDirection::kNone)
        // Strong/held-enough deviation, but this axis/sign has no direction mapped yet
        // (calibrateDirections() hasn't run, or missed this one) -- report nothing rather
        // than guess, same as this file's header comment on the safe default.
        return TiltEvent{};

    return TiltEvent{dir, raw.phase, raw.holdMs};
}

// Edge-function fill (standard barycentric-sign rasterization), bounds-checked against
// the frame buffer like every other direct-pixel draw in this project (see overlay.cpp's
// drawScaleBar()).
static void fillTriangle(uint16_t *frameBuf, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    int minX = std::min({x0, x1, x2}), maxX = std::max({x0, x1, x2});
    int minY = std::min({y0, y1, y2}), maxY = std::max({y0, y1, y2});
    minX = std::max(minX, 0);
    minY = std::max(minY, 0);
    maxX = std::min(maxX, Display::kDisplayWidth - 1);
    maxY = std::min(maxY, Display::kDisplayHeight - 1);

    auto edge = [](int ax, int ay, int bx, int by, int px, int py)
    { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };

    for (int py = minY; py <= maxY; py++)
    {
        for (int px = minX; px <= maxX; px++)
        {
            int w0 = edge(x1, y1, x2, y2, px, py);
            int w1 = edge(x2, y2, x0, y0, px, py);
            int w2 = edge(x0, y0, x1, y1, px, py);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                frameBuf[py * Display::kDisplayWidth + px] = color;
        }
    }
}

void drawTiltArrow(uint16_t *frameBuf, TiltDirection dir, uint16_t color)
{
    constexpr int kCx = Display::kDisplayWidth / 2, kCy = Display::kDisplayHeight / 2;
    constexpr int kW = Display::kDisplayWidth, kH = Display::kDisplayHeight;
    constexpr int kM = kTiltArrowMarginPx, kL = kTiltArrowLengthPx, kHw = kTiltArrowHalfWidthPx;

    switch (dir)
    {
    case TiltDirection::kRight:
        fillTriangle(frameBuf, kW - kM, kCy, kW - kM - kL, kCy - kHw, kW - kM - kL, kCy + kHw, color);
        break;
    case TiltDirection::kLeft:
        fillTriangle(frameBuf, kM, kCy, kM + kL, kCy - kHw, kM + kL, kCy + kHw, color);
        break;
    case TiltDirection::kUp:
        fillTriangle(frameBuf, kCx, kM, kCx - kHw, kM + kL, kCx + kHw, kM + kL, color);
        break;
    case TiltDirection::kDown:
        fillTriangle(frameBuf, kCx, kH - kM, kCx - kHw, kH - kM - kL, kCx + kHw, kH - kM - kL, color);
        break;
    case TiltDirection::kNone:
        break;
    }
}
