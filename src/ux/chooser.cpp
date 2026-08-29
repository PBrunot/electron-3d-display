#include "ux/chooser.h"

#include <cstdio>

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
#include "ux/tilt_gesture.h"
#include "config/visual_constants.h" // kTextColor, kAccentColor, kChooserPollDelayMs, kChooserIdleJumpUs, kCalibLine*, kChooserOption*, kChooserBlinkHalfPeriodMs

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

/**
 * @brief Draw the menu screen -- background + the two blinking option lines.
 *
 * `fullRedraw` gates the (comparatively expensive, ~60-90ms) background decode-and-draw: false
 * on every steady-state poll, since the background never changes on its own between polls --
 * only the option text (recolored, same shape/position every time, so no erase-first needed:
 * font.cpp's glyph rasterizer only ever writes "on" pixels, see drawCharImpl(), so redrawing
 * the same glyphs in a new color can't leave stale pixels behind) and the tilt arrow (handled
 * separately by runChooser() via restoreArrowBackground(), since erasing it needs to know its
 * own last-drawn rectangle, not the whole screen) ever change. true only on the first frame
 * after entering the menu or returning from a viewer, when the frame buffer holds whatever
 * that other screen left.
 *
 * Also re-captures the tilt-arrow-sized background cache (captureArrowBackgrounds()) whenever
 * it redraws the background -- must happen AFTER the background is freshly painted and BEFORE
 * any arrow is drawn on top of it, or the cache would capture stale/arrow pixels instead of
 * the real background.
 */
static void drawChooserScreen(Display &display, bool fullRedraw)
{
    if (fullRedraw)
    {
        drawSplashScreen(display); // no-op (logged) on mount/decode failure, not a crash
        captureArrowBackgrounds(display);
    }

    // Alternate between two colors each half-period (rather than blinking on/off) so the
    // text stays put and flashy the whole time instead of periodically vanishing.
    bool colorA = (esp_timer_get_time() / (int64_t(kChooserBlinkHalfPeriodMs) * 1000)) % 2 == 0;
    uint16_t color = colorA ? kChooserOptionColorA : kChooserOptionColorB;
    drawTextCentered(display, kChooserOption1Y, "UP: Orbitals", color, kFontLarge, kChooserOptionScale);
    drawTextCentered(display, kChooserOption2Y, "DOWN: Elements", color, kFontLarge, kChooserOptionScale);
}

/**
 * @brief Main menu loop: splash background, Up/Down tilt selects a viewer.
 *
 * Direction calibration (or the planar-check skip of it) is main.cpp's call to make before
 * this function is ever entered -- see checkPlanarAtBoot()/calibrateDirections() there; this
 * loop does not repeat it.
 */
void runChooser(Display &display, TiltGestureDetector &tilt)
{
    ESP_LOGI(kChooserTag, "menu ready");

    int64_t lastActivityUs = esp_timer_get_time();

    // needsFullRedraw: true only right after entering this loop or returning from a viewer,
    // when the frame buffer holds something other than the chooser background -- every other
    // iteration, the background is already sitting there from the last full redraw and only
    // the option text (self-erasing, see drawChooserScreen()) and the tilt arrow (erased below
    // via its own last-drawn rect, not a full redraw) need touching. lastArrowDir tracks what's
    // currently drawn so the arrow is only touched on an actual state change (appear/disappear/
    // change direction), not every poll.
    bool needsFullRedraw = true;
    TiltDirection lastArrowDir = TiltDirection::kNone;

    while (true)
    {
        screenshot_pause::checkpoint(); // see screenshot_pause.h -- lets a screenshot capture happen safely
        display.waitForFlushDone();
        drawChooserScreen(display, needsFullRedraw);
        needsFullRedraw = false;

        TiltEvent ev = tilt.poll();
        TiltDirection arrowDir = ev.phase != TiltPhase::kIdle ? ev.direction : TiltDirection::kNone;
        if (arrowDir != lastArrowDir)
        {
            restoreArrowBackground(display, lastArrowDir); // no-op if lastArrowDir is kNone
            if (arrowDir != TiltDirection::kNone)
                drawTiltArrow(display, arrowDir, kAccentColor);
            lastArrowDir = arrowDir;
        }
        display.presentFrame();

        if (ev.phase == TiltPhase::kConfirmed)
        {
            lastActivityUs = esp_timer_get_time();
            if (ev.direction == TiltDirection::kUp)
            {
                ESP_LOGI(kChooserTag, "-> orbital viewer");
                runOrbitalView(display, tilt);
                ESP_LOGI(kChooserTag, "back to menu");
            }
            else if (ev.direction == TiltDirection::kDown)
            {
                ESP_LOGI(kChooserTag, "-> element viewer");
                runAtomView(display, tilt);
                ESP_LOGI(kChooserTag, "back to menu");
            }
            lastActivityUs = esp_timer_get_time();
            needsFullRedraw = true;
            lastArrowDir = TiltDirection::kNone; // the viewer's own screens drew over any arrow too
        }
        else if (esp_timer_get_time() - lastActivityUs > kChooserIdleJumpUs)
        {
            if (randomUnit() < orb_real_t(0.5))
            {
                ESP_LOGI(kChooserTag, "idle 30s+ -- auto-launching orbital viewer");
                runOrbitalView(display, tilt);
            }
            else
            {
                ESP_LOGI(kChooserTag, "idle 30s+ -- auto-launching element viewer");
                runAtomView(display, tilt);
            }
            ESP_LOGI(kChooserTag, "back to menu");
            lastActivityUs = esp_timer_get_time();
            needsFullRedraw = true;
            lastArrowDir = TiltDirection::kNone; // the viewer's own screens drew over any arrow too
        }

        vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
    }
}
