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
 * @brief Draw the menu screen -- background art, the fixed-color toolbar band, the two
 *        blinking option lines, and (while a tilt is held) the direction-cluster arrow.
 *
 * `fullRedraw` gates the (comparatively expensive, ~60-90ms) background decode-and-draw: false
 * on every steady-state poll, since the background never changes on its own between polls.
 * The toolbar band (kChooserBandY down to the screen bottom) is repainted flat every poll
 * regardless -- a plain rect fill, not a decode -- so the option text and arrow cluster always
 * sit on a known, graphics-free backdrop; drawing over the band from scratch each time means
 * nothing here ever needs to preserve or restore whatever was underneath it (unlike the old
 * screen-edge arrow, which had to snapshot/restore the splash artwork it was drawn over).
 */
static void drawChooserScreen(Display &display, bool fullRedraw, TiltEvent ev)
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
}

/**
 * @brief Main menu loop: splash background, Up/Down tilt selects a viewer.
 *
 * Direction calibration (or the planar-check skip of it) is main.cpp's call to make before
 * this function is ever entered -- see checkPlanarAtBoot()/calibrateDirections() there; this
 * loop does not repeat it.
 */
void runChooser(Display &display, GestureSource &tilt)
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
        display.waitForFlushDone();

        TiltEvent ev = tilt.poll();
        drawChooserScreen(display, needsFullRedraw, ev);
        needsFullRedraw = false;
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
        }

        vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
    }
}
