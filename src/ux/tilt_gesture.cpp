#include "ux/tilt_gesture.h"

#include <algorithm>
#include <cmath>

#include "render/display.h"
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
    orb_real_t sumMag = orb_real_t(0), sumMagSq = orb_real_t(0);
    int ok = 0;
    for (int i = 0; i < cfg_.calibrationSamples; i++)
    {
        orb_real_t x, y, z;
        if (imu_.readAccelG(&x, &y, &z))
        {
            sumX += x;
            sumY += y;
            sumZ += z;
            orb_real_t mag = std::sqrt(x * x + y * y + z * z);
            sumMag += mag;
            sumMagSq += mag * mag;
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

    // Stddev of the samples' magnitude: a high value means the board was moving/being
    // handled during calibration, not resting flat, so the averaged baseline may not be
    // trustworthy. Logged as a warning only -- there's no UI yet to ask the user to hold
    // still and retry.
    orb_real_t stdDev = orb_real_t(0);
    if (ok > 1)
    {
        orb_real_t meanMag = sumMag / orb_real_t(ok);
        orb_real_t variance = sumMagSq / orb_real_t(ok) - meanMag * meanMag;
        stdDev = variance > orb_real_t(0) ? std::sqrt(variance) : orb_real_t(0);
    }
    ESP_LOGI(kTiltTag, "calibrated baseline (%d/%d samples ok): x=%.3fg y=%.3fg z=%.3fg, stddev=%.4fg", ok,
             cfg_.calibrationSamples, double(baseX_), double(baseY_), double(baseZ_), double(stdDev));
    if (stdDev > cfg_.calibrationMaxStdDevG)
        ESP_LOGW(kTiltTag, "baseline stddev %.4fg exceeds %.4fg -- board may not have been held still/flat",
                 double(stdDev), double(cfg_.calibrationMaxStdDevG));
}

void TiltGestureDetector::setBaseline(orb_real_t x, orb_real_t y, orb_real_t z)
{
    baseX_ = x;
    baseY_ = y;
    baseZ_ = z;
    ESP_LOGI(kTiltTag, "baseline set directly: x=%.3fg y=%.3fg z=%.3fg", double(x), double(y), double(z));
}

void TiltGestureDetector::setMapping(orb_real_t dirX, orb_real_t dirY, orb_real_t dirZ, TiltDirection dir)
{
    if (dir == TiltDirection::kNone)
        return;
    int idx = int(dir) - 1;
    directionRefs_[idx] = {dirX, dirY, dirZ, true};
    ESP_LOGI(kTiltTag, "mapping set: dir=(%.2f, %.2f, %.2f) -> %s", double(dirX), double(dirY), double(dirZ),
             tiltDirectionName(dir));
}

void TiltGestureDetector::logCalibrationForHardcode() const
{
    // Printed as ready-to-paste C++ -- copy straight into config/hardware_constants.h's matching
    // constant names (see Qmi8658::checkPlanarAtBoot() in imu.cpp for how they're consumed).
    ESP_LOGI(kTiltTag, "--- calibration constants for config/hardware_constants.h ---");
    ESP_LOGI(kTiltTag, "constexpr orb_real_t kDefaultBaselineX = orb_real_t(%.4f);", double(baseX_));
    ESP_LOGI(kTiltTag, "constexpr orb_real_t kDefaultBaselineY = orb_real_t(%.4f);", double(baseY_));
    ESP_LOGI(kTiltTag, "constexpr orb_real_t kDefaultBaselineZ = orb_real_t(%.4f);", double(baseZ_));
    static const char *kNames[kTiltDirectionCount] = {"Left", "Right", "Up", "Down"};
    for (int i = 0; i < kTiltDirectionCount; i++)
    {
        const DirectionRef &ref = directionRefs_[i];
        ESP_LOGI(kTiltTag, "constexpr orb_real_t kDefaultDirRef%sX = orb_real_t(%.4f);", kNames[i], double(ref.x));
        ESP_LOGI(kTiltTag, "constexpr orb_real_t kDefaultDirRef%sY = orb_real_t(%.4f);", kNames[i], double(ref.y));
        ESP_LOGI(kTiltTag, "constexpr orb_real_t kDefaultDirRef%sZ = orb_real_t(%.4f);", kNames[i], double(ref.z));
    }
    ESP_LOGI(kTiltTag, "--- end calibration constants ---");
}

RawTiltEvent TiltGestureDetector::pollRaw()
{
    orb_real_t x, y, z;
    if (!imu_.readAccelG(&x, &y, &z))
    {
        ESP_LOGW(kTiltTag, "IMU read failed, skipping this poll");
        if (!active_)
            return RawTiltEvent{};
        return RawTiltEvent{activeDirX_, activeDirY_, activeDirZ_, TiltPhase::kHolding, nowMs() - holdStartMs_};
    }

    orb_real_t dx = x - baseX_, dy = y - baseY_, dz = z - baseZ_;
    orb_real_t mag = std::sqrt(dx * dx + dy * dy + dz * dz);
    uint32_t now = nowMs();

    if (!active_)
    {
        if (mag < cfg_.thresholdG)
            return RawTiltEvent{};

        // Latch the normalized direction for the whole hold (recomputed continuously, a
        // multi-second hold wouldn't stay perfectly steady) -- see tilt_gesture.h's header
        // comment for why a full 3D unit vector, not a single dominant axis.
        activeDirX_ = dx / mag;
        activeDirY_ = dy / mag;
        activeDirZ_ = dz / mag;
        ESP_LOGI(kTiltTag, "candidate: dir=(%.2f, %.2f, %.2f) mag=%.2fg", double(activeDirX_), double(activeDirY_),
                 double(activeDirZ_), double(mag));

        active_ = true;
        holdStartMs_ = now;
        confirmedFired_ = false;
        return RawTiltEvent{activeDirX_, activeDirY_, activeDirZ_, TiltPhase::kHolding, 0};
    }

    if (mag < cfg_.releaseG)
    {
        ESP_LOGI(kTiltTag, "released dir=(%.2f, %.2f, %.2f) after %ums (mag=%.2fg)", double(activeDirX_),
                 double(activeDirY_), double(activeDirZ_), now - holdStartMs_, double(mag));
        active_ = false;
        return RawTiltEvent{};
    }

    uint32_t heldMs = now - holdStartMs_;
    if (!confirmedFired_ && heldMs >= cfg_.holdConfirmMs)
    {
        confirmedFired_ = true;
        ESP_LOGI(kTiltTag, "CONFIRMED dir=(%.2f, %.2f, %.2f) after %ums", double(activeDirX_), double(activeDirY_),
                 double(activeDirZ_), heldMs);
        return RawTiltEvent{activeDirX_, activeDirY_, activeDirZ_, TiltPhase::kConfirmed, heldMs};
    }
    return RawTiltEvent{activeDirX_, activeDirY_, activeDirZ_, TiltPhase::kHolding, heldMs};
}

TiltEvent TiltGestureDetector::poll()
{
    RawTiltEvent raw = pollRaw();
    if (raw.phase == TiltPhase::kIdle)
        return TiltEvent{};

    // Nearest-direction match via cosine similarity (dot product of unit vectors) against
    // every calibrated reference -- see tilt_gesture.h's header comment for the rationale.
    // Below cfg.minDirectionSimilarity, report nothing rather than the best-of-a-bad-lot guess.
    TiltDirection best = TiltDirection::kNone;
    orb_real_t bestSim = cfg_.minDirectionSimilarity;
    for (int i = 0; i < kTiltDirectionCount; i++)
    {
        if (!directionRefs_[i].valid)
            continue;
        orb_real_t sim =
            raw.dirX * directionRefs_[i].x + raw.dirY * directionRefs_[i].y + raw.dirZ * directionRefs_[i].z;
        if (sim > bestSim)
        {
            bestSim = sim;
            best = TiltDirection(i + 1);
        }
    }
    if (best == TiltDirection::kNone)
        return TiltEvent{};

    return TiltEvent{best, raw.phase, raw.holdMs};
}

// Edge-function fill (standard barycentric-sign rasterization); writePx() bounds-checks
// every pixel like every other direct-pixel draw in this project (see overlay.cpp's
// drawScaleBar()).
static void fillTriangle(Display &display, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
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
                display.writePx(px, py, color);
        }
    }
}

namespace
{
    /// Screen-edge-anchored vertices for drawTiltArrow() -- returns false (vertices untouched)
    /// for kNone.
    bool arrowTriangle(TiltDirection dir, int &x0, int &y0, int &x1, int &y1, int &x2, int &y2)
    {
        constexpr int kCx = Display::kDisplayWidth / 2, kCy = Display::kDisplayHeight / 2;
        constexpr int kW = Display::kDisplayWidth, kH = Display::kDisplayHeight;
        constexpr int kM = kTiltArrowMarginPx, kL = kTiltArrowLengthPx, kHw = kTiltArrowHalfWidthPx;

        switch (dir)
        {
        case TiltDirection::kRight:
            x0 = kW - kM;
            y0 = kCy;
            x1 = x2 = kW - kM - kL;
            y1 = kCy - kHw;
            y2 = kCy + kHw;
            return true;
        case TiltDirection::kLeft:
            x0 = kM;
            y0 = kCy;
            x1 = x2 = kM + kL;
            y1 = kCy - kHw;
            y2 = kCy + kHw;
            return true;
        case TiltDirection::kUp:
            x0 = kCx;
            y0 = kM;
            y1 = y2 = kM + kL;
            x1 = kCx - kHw;
            x2 = kCx + kHw;
            return true;
        case TiltDirection::kDown:
            x0 = kCx;
            y0 = kH - kM;
            y1 = y2 = kH - kM - kL;
            x1 = kCx - kHw;
            x2 = kCx + kHw;
            return true;
        case TiltDirection::kNone:
            return false;
        }
        return false;
    }
} // namespace

void drawTiltArrow(Display &display, TiltDirection dir, uint16_t color)
{
    int x0, y0, x1, y1, x2, y2;
    if (arrowTriangle(dir, x0, y0, x1, y1, x2, y2))
        fillTriangle(display, x0, y0, x1, y1, x2, y2, color);
}

namespace
{
    /// Like arrowTriangle() above, but centered at an arbitrary point with its own size instead
    /// of anchored to a screen edge via the shared kTiltArrow*Px constants -- see
    /// drawTiltArrowAt()'s header comment.
    bool arrowTriangleAt(TiltDirection dir, int cx, int cy, int length, int halfWidth, int &x0, int &y0, int &x1,
                        int &y1, int &x2, int &y2)
    {
        int half = length / 2;
        switch (dir)
        {
        case TiltDirection::kRight:
            x0 = cx + half;
            y0 = cy;
            x1 = x2 = cx - half;
            y1 = cy - halfWidth;
            y2 = cy + halfWidth;
            return true;
        case TiltDirection::kLeft:
            x0 = cx - half;
            y0 = cy;
            x1 = x2 = cx + half;
            y1 = cy - halfWidth;
            y2 = cy + halfWidth;
            return true;
        case TiltDirection::kUp:
            x0 = cx;
            y0 = cy - half;
            y1 = y2 = cy + half;
            x1 = cx - halfWidth;
            x2 = cx + halfWidth;
            return true;
        case TiltDirection::kDown:
            x0 = cx;
            y0 = cy + half;
            y1 = y2 = cy - half;
            x1 = cx - halfWidth;
            x2 = cx + halfWidth;
            return true;
        case TiltDirection::kNone:
            return false;
        }
        return false;
    }
} // namespace

void drawTiltArrowAt(Display &display, TiltDirection dir, int cx, int cy, int length, int halfWidth, uint16_t color)
{
    int x0, y0, x1, y1, x2, y2;
    if (arrowTriangleAt(dir, cx, cy, length, halfWidth, x0, y0, x1, y1, x2, y2))
        fillTriangle(display, x0, y0, x1, y1, x2, y2, color);
}
