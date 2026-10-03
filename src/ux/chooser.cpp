#include "ux/chooser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "views/atom_view.h"
#include "render/camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "render/font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "views/orbital_view.h"
#include "debug/screenshot_pause.h"
#include "render/splash_bitmap.h"
#include "ux/orientation_tracker.h"
#include "ux/remote_command.h"
#include "ux/tilt_gesture.h"
#include "config/visual_constants.h" // kTextColor, kAccentColor, kChooserPollDelayMs, kChooserIdleJumpUs, kCalibLine*, kChooserOption*, kChooserBlinkHalfPeriodMs, kOrientGauge*, kWebHint*
#include "config/network_constants.h" // kWebRemoteEnabled, kWebRemoteSsid

static const char *kChooserTag = "chooser";

struct CalibTarget
{
    TiltDirection dir;
    const char *label;
};

/// Short enough to stay comfortably inside 240px at kFontLarge -- kept to just the direction,
/// with "and hold" split onto its own line below instead of appended to a single long string.
static constexpr CalibTarget kCalibTargets[] = {
    {TiltDirection::kRight, "TILT RIGHT"},
    {TiltDirection::kLeft, "TILT LEFT"},
    {TiltDirection::kUp, "TILT UP"},
    {TiltDirection::kDown, "TILT DOWN"},
};
static constexpr int kCalibTargetCount = sizeof(kCalibTargets) / sizeof(kCalibTargets[0]);

/**
 * @brief Guided sequence prompting the user to tilt-and-hold each of Right/Left/Up/Down in
 *        turn, recording each one's deviation vector as that direction's reference.
 *
 * Run at boot before the real menu appears (see runChooser()). For each target: poll until a
 * confirmed hold is captured (showing live hold progress), record the mapping via
 * tilt.setMapping(), then wait for release back to baseline before moving to the next target
 * so the same still-held tilt doesn't immediately roll into the next target's detection loop.
 */
void calibrateDirections(Display &display, TiltGestureDetector &tilt)
{
    ESP_LOGI(kChooserTag, "starting direction calibration (%d targets)", kCalibTargetCount);

    for (const CalibTarget &target : kCalibTargets)
    {
        RawTiltEvent raw{};
        while (raw.phase != TiltPhase::kConfirmed)
        {
            display.waitForFlushDone();
            display.clearScreen();
            drawTextCentered(display, kCalibLineY0, "Calibration", kTextColor, kFontLarge);
            drawTextCentered(display, kCalibLineY0 + kCalibLineSpacing, target.label, kTextColor, kFontLarge);
            if (raw.phase == TiltPhase::kHolding)
            {
                char progress[24];
                std::snprintf(progress, sizeof(progress), "%.1fs / 1.0s", double(raw.holdMs) / 1000.0);
                drawTextCentered(display, kCalibLineY0 + 2 * kCalibLineSpacing, progress, kAccentColor,
                                 kFontLarge);
            }
            else
            {
                drawTextCentered(display, kCalibLineY0 + 2 * kCalibLineSpacing, "and hold", kTextColor,
                                 kFontLarge);
            }
            display.presentFrame();

            raw = tilt.pollRaw();
            vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
        }

        tilt.setMapping(raw.dirX, raw.dirY, raw.dirZ, target.dir);

        RawTiltEvent release = raw;
        while (release.phase != TiltPhase::kIdle)
        {
            display.waitForFlushDone();
            display.clearScreen();
            drawTextCentered(display, kCalibLineY0, "Calibration", kTextColor, kFontLarge);
            drawTextCentered(display, kCalibLineY0 + kCalibLineSpacing, target.label, kAccentColor, kFontLarge);
            drawTextCentered(display, kCalibLineY0 + 2 * kCalibLineSpacing, "OK - RELEASE", kAccentColor,
                             kFontLarge);
            display.presentFrame();

            release = tilt.pollRaw();
            vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
        }
    }

    ESP_LOGI(kChooserTag, "direction calibration complete");
}

namespace
{
    /// Plain Bresenham line -- writePx()-bounds-checked per pixel via Display, so no manual
    /// clipping needed here. Nothing else in the codebase needs an arbitrary-angle line yet
    /// (tilt_gesture.cpp's arrows only ever point along one of 4 fixed cardinal directions, so
    /// it draws filled triangles directly instead) -- kept local to this file rather than
    /// factored out until a second caller actually needs it.
    void drawLine(Display &display, int x0, int y0, int x1, int y1, uint16_t color)
    {
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        while (true)
        {
            display.writePx(x0, y0, color);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 >= dy)
            {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    }

    /// One dial: a pivot dot, a needle from the pivot pointing at `angleRad` (0 = straight up,
    /// positive = clockwise, matching a clock face), and a kFontSmall label centered under it.
    /// Angle is NOT wrapped/clamped here -- yaw sweeps past +-pi and the needle just keeps
    /// spinning, which is the point (a stuck needle reads as "not moving" just as clearly as a
    /// wrong-signed one reads as "moving the wrong way").
    void drawOrientationDial(Display &display, int cx, int cy, orb_real_t angleRad, const char *label)
    {
        int tipX = cx + int(orb_real_t(kOrientGaugeRadiusPx) * std::sin(angleRad));
        int tipY = cy - int(orb_real_t(kOrientGaugeRadiusPx) * std::cos(angleRad));
        drawLine(display, cx, cy, tipX, tipY, kOrientGaugeNeedleColor);
        display.writePx(cx, cy, kOrientGaugeNeedleColor);

        int labelX = cx - textWidth(label, kFontSmall) / 2;
        drawText(display, labelX, kOrientGaugeLabelY, label, kOrientGaugeLabelColor, kFontSmall);
    }

    /// Top-left tilt/roll/yaw needle cluster -- see kOrientGauge* in visual_constants.h for the
    /// full rationale. No-op if `orientation` is nullptr (CYD: no IMU, see main.cpp).
    void drawOrientationGauges(Display &display, OrientationTracker *orientation)
    {
        if (orientation == nullptr)
            return;

        for (int y = kOrientGaugeAreaY; y < kOrientGaugeAreaY + kOrientGaugeAreaH; y++)
            for (int x = kOrientGaugeAreaX; x < kOrientGaugeAreaX + kOrientGaugeAreaW; x++)
                display.writePx(x, y, kOrientGaugeBgColor);

        int cx = kOrientGaugeFirstCx;
        drawOrientationDial(display, cx, kOrientGaugeCy, orientation->tiltRad(), "TILT");
        cx += kOrientGaugeSpacingPx;
        drawOrientationDial(display, cx, kOrientGaugeCy, orientation->rollRad(), "ROLL");
        cx += kOrientGaugeSpacingPx;
        drawOrientationDial(display, cx, kOrientGaugeCy, orientation->yawRad(), "YAW");
    }

    /// Top-right "WiFi / <ssid>" hint -- see kWebHint* in visual_constants.h.
    void drawWebRemoteHint(Display &display)
    {
        if constexpr (!kWebRemoteEnabled)
            return;

        constexpr const char *kHeading = "WiFi";
        int width = std::max(textWidth(kHeading, kFontSmall), textWidth(kWebRemoteSsid, kFontSmall));
        int lineAdvance = kFontSmall.lineAdvance;
        for (int y = kWebHintY - kWebHintPadPx; y < kWebHintY + 2 * lineAdvance + kWebHintPadPx; y++)
            for (int x = kWebHintRightX - width - kWebHintPadPx; x <= kWebHintRightX + kWebHintPadPx; x++)
                display.writePx(x, y, kWebHintBgColor);
        drawText(display, kWebHintRightX - textWidth(kHeading, kFontSmall), kWebHintY, kHeading, kAccentColor,
                 kFontSmall);
        drawText(display, kWebHintRightX - textWidth(kWebRemoteSsid, kFontSmall), kWebHintY + lineAdvance,
                 kWebRemoteSsid, kWebHintColor, kFontSmall);
    }

    /// Launches the viewer a pending web-remote request asks for (net/web_remote.cpp), opened
    /// directly on the requested element/orbital. Returns true if a viewer ran (and has since
    /// returned). kNext/kPrev/kMenu have nothing to act on from the menu and are dropped.
    bool launchRemoteRequest(Display &display, GestureSource &tilt, OrientationTracker *orientation)
    {
        remote::Request request = remote::take();
        switch (request.cmd)
        {
        case remote::Command::kShowElement:
            ESP_LOGI(kChooserTag, "web remote -> element viewer (Z=%d)", request.arg);
            runAtomView(display, tilt, orientation, request.arg);
            return true;
        case remote::Command::kShowOrbital:
            ESP_LOGI(kChooserTag, "web remote -> orbital viewer (preset %d)", request.arg);
            runOrbitalView(display, tilt, orientation, request.arg);
            return true;
        case remote::Command::kDissect:
            ESP_LOGI(kChooserTag, "web remote -> element viewer, then dissect");
            remote::postIfEmpty(request); // picked up by runAtomView()'s loop once its intro is done
            runAtomView(display, tilt, orientation);
            return true;
        case remote::Command::kNext:
        case remote::Command::kPrev:
        case remote::Command::kMenu:
        case remote::Command::kNone:
            break;
        }
        return false;
    }
} // namespace

/**
 * @brief Draw the menu screen -- background art, the fixed-color toolbar band, the two
 *        blinking option lines, (while a tilt is held) the direction-cluster arrow, and (IMU
 *        boards only) the top-left tilt/roll/yaw orientation gauges.
 *
 * `fullRedraw` gates the (comparatively expensive, ~60-90ms) background decode-and-draw: false
 * on every steady-state poll, since the background never changes on its own between polls.
 * The toolbar band (kChooserBandY down to the screen bottom) and the orientation gauge area
 * are both repainted flat every poll regardless -- plain rect fills, not a decode -- so
 * nothing drawn on top of either ever needs to preserve or restore whatever was underneath it
 * (unlike the old screen-edge arrow, which had to snapshot/restore the splash artwork it was
 * drawn over).
 */
static void drawChooserScreen(Display &display, bool fullRedraw, TiltEvent ev, OrientationTracker *orientation)
{
    if (fullRedraw)
        drawSplashScreen(display); // no-op (logged) on mount/decode failure, not a crash

    // Alternate between two colors each half-period (rather than blinking on/off) so the
    // text stays put and flashy the whole time instead of periodically vanishing.
    bool colorA = (esp_timer_get_time() / (int64_t(kChooserBlinkHalfPeriodMs) * 1000)) % 2 == 0;
    uint16_t color = colorA ? kChooserOptionColorA : kChooserOptionColorB;

    drawTextCentered(display, Display::kDisplayHeight - 60, "UP: Orbitals", color, kFontHuge, kChooserOptionScale);
    drawTextCentered(display, Display::kDisplayHeight - 30, "DOWN: Elements", color, kFontHuge, kChooserOptionScale);

    if (ev.phase != TiltPhase::kIdle)
        drawTiltArrowAt(display, ev.direction, kChooserArrowClusterCx, kChooserArrowClusterCy, kChooserArrowLengthPx,
                        kChooserArrowHalfWidthPx, kAccentColor);

    drawOrientationGauges(display, orientation);
    drawWebRemoteHint(display);
}

/**
 * @brief Main menu loop: splash background, Up/Down tilt selects a viewer.
 *
 * Direction calibration (or the planar-check skip of it) is main.cpp's call to make before
 * this function is ever entered -- see checkPlanarAtBoot()/calibrateDirections() there; this
 * loop does not repeat it.
 */
void runChooser(Display &display, GestureSource &tilt, OrientationTracker *orientation)
{
    ESP_LOGI(kChooserTag, "menu ready");

    int64_t lastActivityUs = esp_timer_get_time();

    // needsFullRedraw: true only right after entering this loop or returning from a viewer,
    // when the frame buffer holds something other than the chooser background -- every other
    // iteration, the background art is already sitting there from the last full redraw; the
    // toolbar band/text/arrow are cheap enough to repaint every poll regardless (see
    // drawChooserScreen()), so there's no separate arrow-erase state to track here anymore.
    bool needsFullRedraw = true;

    while (true)
    {
        screenshot_pause::checkpoint(); // see screenshot_pause.h -- lets a screenshot capture happen safely

        // Checked before drawing, so a request a viewer handed back on its way out (see
        // remote::postIfEmpty()) relaunches straight into the other viewer without first
        // paying the menu's full background redraw.
        if (launchRemoteRequest(display, tilt, orientation))
        {
            ESP_LOGI(kChooserTag, "back to menu");
            lastActivityUs = esp_timer_get_time();
            needsFullRedraw = true;
            continue;
        }

        display.waitForFlushDone();
        if (needsFullRedraw)
            remote::publishState({remote::ViewMode::kMenu, 0});

        TiltEvent ev = tilt.poll();
        if (orientation != nullptr)
            orientation->update(); // feeds drawChooserScreen()'s top-left tilt/roll/yaw gauges
        drawChooserScreen(display, needsFullRedraw, ev, orientation);
        needsFullRedraw = false;
        display.presentFrame();

        if (ev.phase == TiltPhase::kConfirmed)
        {
            lastActivityUs = esp_timer_get_time();
            if (ev.direction == TiltDirection::kUp)
            {
                ESP_LOGI(kChooserTag, "-> orbital viewer");
                runOrbitalView(display, tilt, orientation);
                ESP_LOGI(kChooserTag, "back to menu");
            }
            else if (ev.direction == TiltDirection::kDown)
            {
                ESP_LOGI(kChooserTag, "-> element viewer");
                runAtomView(display, tilt, orientation);
                ESP_LOGI(kChooserTag, "back to menu");
            }
            lastActivityUs = esp_timer_get_time();
            needsFullRedraw = true;
        }
        else if (esp_timer_get_time() - lastActivityUs > kChooserIdleJumpUs)
        {
            if (randomUnit() < orb_real_t(0.5))
            {
                ESP_LOGI(kChooserTag, "idle 30s+ -- auto-launching orbital viewer");
                runOrbitalView(display, tilt, orientation);
            }
            else
            {
                ESP_LOGI(kChooserTag, "idle 30s+ -- auto-launching element viewer");
                runAtomView(display, tilt, orientation);
            }
            ESP_LOGI(kChooserTag, "back to menu");
            lastActivityUs = esp_timer_get_time();
            needsFullRedraw = true;
        }

        vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
    }
}
