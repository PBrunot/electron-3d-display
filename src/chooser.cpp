#include "chooser.h"

#include <algorithm>
#include <cstdio>

#include "atom_view.h"
#include "esp_log.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_view.h"
#include "tilt_gesture.h"

static const char *kChooserTag = "chooser";

constexpr uint16_t kChooserTextColor = Display::kColorWhite;
constexpr uint16_t kChooserArrowColor = Display::packColor565(255, 210, 60);
constexpr int kChooserPollDelayMs = 30;

static void drawCentered(uint16_t *frameBuf, int y, const char *text, uint16_t color, const Font &font)
{
    int x = (Display::kDisplayWidth - textWidth(text, font)) / 2;
    drawText(frameBuf, x, y, text, color, font);
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

// Short enough to stay comfortably inside 240px at kFontLarge (see calibrateDirections()'s
// "use the biggest font" requirement) -- kept to just the direction, with "and hold" split
// onto its own kFontLarge line below instead of appended to a single long string.
static constexpr CalibTarget kCalibTargets[] = {
    {TiltDirection::kRight, "TILT RIGHT"},
    {TiltDirection::kLeft, "TILT LEFT"},
    {TiltDirection::kUp, "TILT UP"},
    {TiltDirection::kDown, "TILT DOWN"},
};
static constexpr int kCalibTargetCount = sizeof(kCalibTargets) / sizeof(kCalibTargets[0]);

// Every calibration-screen line uses kFontLarge (per feedback: kFontSmall read as "too
// small" on the panel), spaced kFontLarge.lineAdvance*~1.6 apart -- more than a bare
// lineAdvance gap, since these are separate short standalone lines, not wrapped body text.
constexpr int kCalibLineY0 = 60, kCalibLineSpacing = 36;

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

        // Wait for release back to baseline before the next target, so the same
        // still-held tilt doesn't immediately roll into the next target's detection loop.
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
    clearScreen(frameBuf);

    // "the choose screen font is much too small - use the biggest font size" (feedback,
    // 2026-08-17) -- kFontLarge throughout, matching calibrateDirections()'s own screens
    // (same earlier feedback: "kSmallFont read as too small on the panel"). Spacing widened
    // to kCalibLineSpacing to match too -- kFontLarge is taller than kFontSmall, so the old
    // 16px gap between the two menu lines would now overlap.
    const char *title = "electron-3d-display";
    drawCentered(frameBuf, 90, title, kChooserTextColor, kFontLarge);
    drawCentered(frameBuf, 120, "Tilt UP: Orbitals", kChooserTextColor, kFontLarge);
    drawCentered(frameBuf, 120 + kCalibLineSpacing, "Tilt DOWN: Elements", kChooserTextColor, kFontLarge);
}

void runChooser(Display &display, TiltGestureDetector &tilt)
{
    calibrateDirections(display, tilt);
    ESP_LOGI(kChooserTag, "menu ready");

    while (true)
    {
        display.waitForFlushDone();
        drawChooserScreen(display.getFrameBuf());

        TiltEvent ev = tilt.poll();
        if (ev.phase != TiltPhase::kIdle)
            drawTiltArrow(display.getFrameBuf(), ev.direction, kChooserArrowColor);
        display.presentFrame();

        if (ev.phase == TiltPhase::kConfirmed)
        {
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
        }

        vTaskDelay(pdMS_TO_TICKS(kChooserPollDelayMs));
    }
}
