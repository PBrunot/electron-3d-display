#include "chooser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "atom_view.h"
#include "esp_log.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_view.h"
#include "splash_bitmap.h"
#include "tilt_gesture.h"

static const char *kChooserTag = "chooser";

constexpr uint16_t kChooserTextColor = Display::kColorWhite;
constexpr uint16_t kChooserArrowColor = Display::packColor565(255, 210, 60);
// "a title in electric blu: ATOM CUBE" (feedback, 2026-08-17).
constexpr uint16_t kChooserTitleColor = Display::packColor565(0, 150, 255);
constexpr int kChooserPollDelayMs = 30;

// "no the chooser screen shall have the same fixed splash screen background - no animation
// needed" (feedback, 2026-08-17) -- the SAME splash_bitmap.h image main.cpp shows at boot,
// at full brightness (not the dimmed copy an earlier pass here tried), held static behind
// the menu text -- replaces the original rotating-orbital-point-cloud background entirely.

static void drawCentered(uint16_t *frameBuf, int y, const char *text, uint16_t color, const Font &font)
{
    int x = (Display::kDisplayWidth - textWidth(text, font)) / 2;
    drawText(frameBuf, x, y, text, color, font);
}

/** Like drawCentered(), but through drawTextScaled()/textWidthScaled() -- "Options should be
 * with a bigger font" (feedback, 2026-08-17). */
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

// "Options should be with a bigger font" (feedback, 2026-08-17) -- kChooserOptionScale=2 on
// top of kFontLarge. Text shortened from "Tilt UP: Orbitals"/"Tilt DOWN: Elements" (the
// "Tilt" prefix is redundant -- the arrow that appears while actually tilting already says
// so) since at scale 2 the full phrases (measured via font.cpp's kLargeWidths: 226px/286px)
// don't fit the 240px width -- "DOWN: Elements" alone is already a tight 230px/240px fit.
constexpr int kChooserOptionScale = 2;
constexpr int kChooserTitleY = 50, kChooserOption1Y = 105, kChooserOption2Y = 165;

static void drawChooserScreen(uint16_t *frameBuf)
{
    // Splash image, copied in first (at full brightness, unmodified -- "the chooser screen
    // shall have the same fixed splash screen background", feedback 2026-08-17) so the menu
    // text composites on top of it (plain overwrite, no blending, matching every other draw
    // function in this project). Static -- no per-frame animation, so this is just a memcpy
    // straight out of the generated array every frame (cheap enough not to bother caching).
    std::memcpy(frameBuf, kSplashBitmapData, Display::kDisplayWidth * Display::kDisplayHeight * sizeof(uint16_t));

    drawCentered(frameBuf, kChooserTitleY, "ATOM CUBE", kChooserTitleColor, kFontLarge);
    drawCenteredScaled(frameBuf, kChooserOption1Y, "UP: Orbitals", kChooserTextColor, kFontLarge,
                        kChooserOptionScale);
    drawCenteredScaled(frameBuf, kChooserOption2Y, "DOWN: Elements", kChooserTextColor, kFontLarge,
                        kChooserOptionScale);
}

void runChooser(Display &display, TiltGestureDetector &tilt)
{
    // Calibration (or the planar-check skip of it) is main.cpp's call to make, before this
    // function is ever entered -- see checkPlanarAtBoot()/calibrateDirections() there. This
    // used to unconditionally re-run calibrateDirections() here too (so every boot ran the
    // full interactive sequence TWICE in a row, regardless), which silently defeated the
    // planar-check skip: main.cpp would skip its copy, then land right back in this one.
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
