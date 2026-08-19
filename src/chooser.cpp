#include "chooser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "atom_view.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_view.h"
#include "screenshot_pause.h"
#include "splash_bitmap.h"
#include "tilt_gesture.h"

static const char *kChooserTag = "chooser";

// ============================================================================================
// Tunable constants
// ============================================================================================

constexpr uint16_t kChooserTextColor = Display::kColorWhite;
constexpr uint16_t kChooserArrowColor = Display::packColor565(255, 210, 60);
constexpr int kChooserPollDelayMs = 30; ///< Poll/redraw cadence for both the menu and calibration loops.

/// Idle time on the menu (no confirmed tilt-hold) before it auto-launches a demo viewer, coin-
/// flipping between orbitals and elements -- shorter than either viewer's own kIdleJumpUs
/// (60s) since the splash is meant to be a passive attract screen, not something visitors are
/// expected to interact with first.
constexpr int64_t kChooserIdleJumpUs = 30'000'000;

/// Text scale for the "and hold"/direction-name calibration lines, on top of kFontLarge.
constexpr int kCalibLineY0 = 60;      ///< Y of the first calibration line.
constexpr int kCalibLineSpacing = 36; ///< Vertical gap between calibration lines -- more than a bare
                                       ///< lineAdvance since these are separate standalone lines, not wrapped body text.

/// Menu option text scale, on top of kFontLarge.
constexpr int kChooserOptionScale = 2;
constexpr int kChooserOption1Y = 105; ///< Y of the "UP: Orbitals" line.
constexpr int kChooserOption2Y = 165; ///< Y of the "DOWN: Elements" line.

// ============================================================================================

static void drawCentered(uint16_t *frameBuf, int y, const char *text, uint16_t color, const Font &font)
{
    int x = (Display::kDisplayWidth - textWidth(text, font)) / 2;
    drawText(frameBuf, x, y, text, color, font);
}

/// Like drawCentered(), but through drawTextScaled()/textWidthScaled() for a larger option
/// font.
static void drawCenteredScaled(uint16_t *frameBuf, int y, const char *text, uint16_t color, const Font &font,
                               int scale)
{
    int x = (Display::kDisplayWidth - textWidthScaled(text, font, scale)) / 2;
    drawTextScaled(frameBuf, x, y, text, color, font, scale);
}

static void clearScreen(uint16_t *frameBuf)
{
    std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
}

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
            uint16_t *frameBuf = display.getFrameBuf();
            clearScreen(frameBuf);
            drawCentered(frameBuf, kCalibLineY0, "Calibration", kChooserTextColor, kFontLarge);
            drawCentered(frameBuf, kCalibLineY0 + kCalibLineSpacing, target.label, kChooserTextColor, kFontLarge);
            if (raw.phase == TiltPhase::kHolding)
            {
                char progress[24];
                std::snprintf(progress, sizeof(progress), "%.1fs / 1.0s", double(raw.holdMs) / 1000.0);
                drawCentered(frameBuf, kCalibLineY0 + 2 * kCalibLineSpacing, progress, kChooserArrowColor,
                             kFontLarge);
            }
            else
            {
                drawCentered(frameBuf, kCalibLineY0 + 2 * kCalibLineSpacing, "and hold", kChooserTextColor,
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
            uint16_t *frameBuf = display.getFrameBuf();
            clearScreen(frameBuf);
            drawCentered(frameBuf, kCalibLineY0, "Calibration", kChooserTextColor, kFontLarge);
            drawCentered(frameBuf, kCalibLineY0 + kCalibLineSpacing, target.label, kChooserArrowColor, kFontLarge);
            drawCentered(frameBuf, kCalibLineY0 + 2 * kCalibLineSpacing, "OK - RELEASE", kChooserArrowColor,
                         kFontLarge);
            display.presentFrame();

            release = tilt.pollRaw();
            vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
        }
    }

    ESP_LOGI(kChooserTag, "direction calibration complete");
}

static void drawChooserScreen(uint16_t *frameBuf)
{
    // Static splash image (same one main.cpp shows at boot), copied in first at full
    // brightness so the menu text composites on top of it (plain overwrite, no blending,
    // matching every other draw function in this project). No per-frame animation, so this
    // is just a memcpy straight out of the generated array every frame.
    std::memcpy(frameBuf, kSplashBitmapData, Display::kDisplayWidth * Display::kDisplayHeight * sizeof(uint16_t));

    drawCenteredScaled(frameBuf, kChooserOption1Y, "UP: Orbitals", kChooserTextColor, kFontLarge,
                       kChooserOptionScale);
    drawCenteredScaled(frameBuf, kChooserOption2Y, "DOWN: Elements", kChooserTextColor, kFontLarge,
                       kChooserOptionScale);
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

    while (true)
    {
        screenshot_pause::checkpoint(); // see screenshot_pause.h -- lets a screenshot capture happen safely
        display.waitForFlushDone();
        drawChooserScreen(display.getFrameBuf());

        TiltEvent ev = tilt.poll();
        if (ev.phase != TiltPhase::kIdle)
            drawTiltArrow(display.getFrameBuf(), ev.direction, kChooserArrowColor);
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
        }

        vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
    }
}
