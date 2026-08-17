#include "atom_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "element_names_it.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "overlay.h"
#include "slater.h"
#include "ticker.h"

static const char* kAtomViewTag = "atom_view";

// Shared "fun accent" color for the two scrolling tickers below (element-switch Italian
// name, dissection-intro sentence) and the dissection view's big shell notation --
// matches tilt_gesture.h's arrow color, tying this project's few highlight-colored UI
// moments together instead of inventing a new one per feature.
constexpr uint16_t kAccentColor = Display::packColor565(255, 210, 60);

void AtomPresetState::load(int zIn) {
    ESP_LOGI(kAtomViewTag, "loading Z=%d (%s)...", zIn, elementSymbol(zIn));
    int64_t startUs = esp_timer_get_time();

    config = buildAtomPointCloud(zIn, points, kAtomViewNumPoints, kAtomCloudSeed);
    OuterSubshell outer = outerSubshellRRef(points, kAtomViewNumPoints, config);
    colorizeAtomPoints(points, kAtomViewNumPoints, outer, colors);

    AtomScale scale = scaleForAtom(outer.rRef);
    baseScale = scale.baseScale;
    zoomAmplitude = scale.zoomAmplitude;
    z = zIn;

    int64_t buildMs = (esp_timer_get_time() - startUs) / 1000;
    ESP_LOGI(kAtomViewTag, "%s loaded in %lldms, outer=%d%c, outerRBohr=%.2f, scale=%.1f, outerRPx=%.1f",
             elementSymbol(zIn), buildMs, outer.n, subshellLabelChar(outer.ell), double(outer.rRef),
             double(baseScale), double(outer.rRef * baseScale));
}

void drawAtomTitle(uint16_t* frameBuf, int x, int y, int z, const ElectronConfig& config, uint16_t textColor,
                    const Font& font) {
    int cursorX = x, cursorY = y;

    auto drawSegment = [&](const char* segment, uint16_t color) {
        int segWidth = textWidth(segment, font);
        if (cursorX > x && cursorX + segWidth > Display::kDisplayWidth) {
            cursorX = x;
            cursorY += font.lineAdvance;
        }
        cursorX = drawText(frameBuf, cursorX, cursorY, segment, color, font);
    };

    char headSeg[24];
    std::snprintf(headSeg, sizeof(headSeg), "%s (Z=%d) ", elementSymbol(z), z);
    drawSegment(headSeg, textColor);

    for (int i = 0; i < config.count; i++) {
        int n = config.subshells[i].n, ell = config.subshells[i].ell, occ = config.subshells[i].occ;
        char segBuf[8];
        std::snprintf(segBuf, sizeof(segBuf), "%d%c%d ", n, subshellLabelChar(ell), occ);
        const uint8_t* base = shellBaseRgb(n);
        drawSegment(segBuf, Display::packColor565(base[0], base[1], base[2]));
    }
}

// --- Element-switch intro ticker (Right/Left tilt-hold) -------------------------------

namespace {
constexpr int kElementIntroNameScale = 2;
constexpr int kElementIntroSymbolScale = 6; // big, static, pale watermark behind the scrolling name
// Light/muted on purpose -- it's a background, not the star of the animation; the
// scrolling name (kAccentColor) needs to stay the eye's focus on top of it.
constexpr uint16_t kElementIntroSymbolColor = Display::packColor565(90, 90, 100);

/**
 * Scroll `nameIt` (the element's Italian name) right-to-left over a big, static, pale
 * background watermark of `symbol` (its chemical symbol), with a static "Z=<z>" caption
 * underneath the name -- shown before atom_view.cpp switches to a new element. Bespoke
 * (not ticker.h's plain scrollTextOnce()) since it composites three layers instead of one
 * plain scrolling line.
 */
void scrollElementIntro(Display& display, const char* nameIt, int z, const char* symbol, uint16_t nameColor) {
    char zLabel[16];
    std::snprintf(zLabel, sizeof(zLabel), "Z=%d", z);

    int symbolWidth = textWidthScaled(symbol, kFontLarge, kElementIntroSymbolScale);
    int symbolX = (Display::kDisplayWidth - symbolWidth) / 2;
    int symbolY = (Display::kDisplayHeight - kFontLarge.height * kElementIntroSymbolScale) / 2;

    int nameY = 90;
    int zY = nameY + kFontLarge.height * kElementIntroNameScale + 6;
    int zX = (Display::kDisplayWidth - textWidth(zLabel, kFontLarge)) / 2;

    int nameWidth = textWidthScaled(nameIt, kFontLarge, kElementIntroNameScale);
    int x = Display::kDisplayWidth;
    int endX = -nameWidth;
    while (x > endX) {
        display.waitForFlushDone();
        uint16_t* frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
        drawTextScaled(frameBuf, symbolX, symbolY, symbol, kElementIntroSymbolColor, kFontLarge,
                        kElementIntroSymbolScale);
        drawTextScaled(frameBuf, x, nameY, nameIt, nameColor, kFontLarge, kElementIntroNameScale);
        drawText(frameBuf, zX, zY, zLabel, nameColor, kFontLarge);
        display.presentFrame();

        x -= kTickerDefaultPxPerFrame;
    }
}
} // namespace

// --- On-device shell dissection (Down tilt-hold) -- see atom_view.h's header comment ---
//
// Runs as ONE self-contained blocking sequence once triggered (matching
// pc/atom_view_pc.py's own D-key behavior) -- NOT one dissect step per Down-hold. A single
// Down-hold peels through every occupied subshell automatically (eased zoom + a real-time
// hold on each, continuously tumbling throughout) and returns to the full atom at the end,
// with no further gesture needed mid-sequence.

namespace {
DissectionEntry dissectPlan[kMaxConfigSubshells];
int dissectPlanCount = 0;

// Matches pc/atom_view_pc.py's DISSECT_SHADE_GRAY exactly -- the one dissection-view
// constant this device-path simplification still shares with the PC version.
constexpr uint16_t kDissectDimColor = Display::packColor565(70, 70, 70);

// Matches pc/atom_view_pc.py's DISSECT_HOLD_SECONDS=2 -- real-time (esp_timer), not a
// frame count, so the hold duration is the same regardless of the achieved render FPS.
constexpr int64_t kDissectHoldUs = 2 * 1000 * 1000;

void refreshDissectPlan(const AtomPresetState& preset) {
    dissectPlanCount = subshellDissectionPlan(preset.points, kAtomViewNumPoints, preset.config, dissectPlan);
}

int rankInPlan(int n, int ell) {
    for (int i = 0; i < dissectPlanCount; i++)
        if (dissectPlan[i].n == n && dissectPlan[i].ell == ell)
            return i;
    return -1; // every point's subshell is in `config`, so this shouldn't happen for a real cloud
}

/**
 * Compact `points`/`colors` IN PLACE down to the subset visible at dissect level `level`
 * (1..dissectPlanCount), operating on (and shrinking) whatever `count` entries are
 * currently valid -- deliberately NOT a second kAtomViewNumPoints-sized pair of buffers:
 * on real hardware, two full extra 3000-point AtomPoint/color arrays (~66KB of .bss) left
 * too little contiguous internal DMA-capable RAM for Display::Display()'s 112.5KB frame
 * buffer allocation, which aborted at boot. Point order doesn't matter for rendering, so
 * compacting the SAME array in place is safe -- the sequence rebuilds the full cloud fresh
 * (preset.load()) at the end anyway, undoing the compaction.
 *
 * Subshells outer of dissectPlan[level-1] are dropped (peeled away); dissectPlan[level-1]
 * itself (the newly-revealed outermost remaining shell) draws at full shell color; every
 * deeper shell draws flat gray (kDissectDimColor). Returns the count remaining.
 */
int compactDissectLevelInPlace(AtomPoint* points, uint16_t* colors, int count, int level) {
    int keepRank = level - 1;
    int written = 0;
    for (int i = 0; i < count; i++) {
        int rank = rankInPlan(points[i].n, points[i].ell);
        if (rank < keepRank)
            continue;
        points[written] = points[i];
        if (rank == keepRank) {
            const uint8_t* base = shellBaseRgb(points[written].n);
            colors[written] = Display::packColor565(base[0], base[1], base[2]);
        } else {
            colors[written] = kDissectDimColor;
        }
        written++;
    }
    return written;
}

// Feedback: "name of the electron shell should be bigger while dissecting" -- kFontLarge
// is already this project's biggest baked font, so `bigLabel` (just "<n><subshell letter>",
// e.g. "2p" -- always 2 chars, see slater.h's subshellLabelChar()) goes through
// font.h's drawTextScaled() instead. `caption` (the fuller "Shell 2p (2/5)" line) stays
// plain-size underneath it -- scaling THAT up too would overflow the 240px width.
constexpr int kDissectBigScale = 3;

/** Draw `bigLabel` scaled kDissectBigScale x, then `caption` underneath at plain size,
 * both starting at (x, y) -- shared by the eased leg's flyOver() title callback and
 * renderDissectFrame()'s real-time hold below, so both look identical. */
void drawDissectTitle(uint16_t* frameBuf, int x, int y, uint16_t color, const char* bigLabel, const char* caption) {
    drawTextScaled(frameBuf, x, y, bigLabel, color, kFontLarge, kDissectBigScale);
    drawText(frameBuf, x, y + kFontLarge.height * kDissectBigScale + 4, caption, color, kFontLarge);
}

/** Render one frame at a fixed `scale` -- shared by the eased leg (via flyOver()) and the
 * real-time hold below, which isn't eased so doesn't go through flyOver(). */
void renderDissectFrame(Display& display, const AtomPoint* points, const uint16_t* colors, int count,
                         uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, const CameraState& camera,
                         orb_real_t scale, const char* bigLabel, const char* caption) {
    display.waitForFlushDone();
    renderScene(display.getFrameBuf(), points, colors, count, protonColor, camera, scale);
    drawDissectTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor, bigLabel, caption);
    drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
    display.presentFrame();
}

/**
 * Automatically peel through every occupied subshell, outer to inner: for each level, ease
 * the camera in (flyOver()) to frame that subshell (scaleForAtom(active.rRef)) with its
 * label, hold for kDissectHoldUs while continuing to tumble, then move to the next level.
 * Ends by rebuilding the full cloud fresh (preset.load()) and easing back out to
 * preset.baseScale. Blocking -- no tilt polling during the sequence, matching
 * pc/atom_view_pc.py's own one-shot blocking sequence (no abort gesture defined here).
 */
void runDissectionSequence(Display& display, AtomPresetState& preset, CameraState& camera, uint16_t protonColor,
                            uint16_t textColor, uint16_t scaleBarColor) {
    char introText[48];
    std::snprintf(introText, sizeof(introText), "Configurazione elettronica di %s", elementSymbol(preset.z));
    scrollTextOnce(display, introText, kFontLarge, 1, kAccentColor, 110);

    orb_real_t scale = preset.baseScale;
    int count = kAtomViewNumPoints;

    for (int level = 1; level <= dissectPlanCount; level++) {
        DissectionEntry active = dissectPlan[level - 1];
        count = compactDissectLevelInPlace(preset.points, preset.colors, count, level);
        AtomScale s = scaleForAtom(active.rRef);

        char bigLabel[8];
        std::snprintf(bigLabel, sizeof(bigLabel), "%d%c", active.n, subshellLabelChar(active.ell));
        char caption[40];
        std::snprintf(caption, sizeof(caption), "Shell %d%c (%d/%d)", active.n, subshellLabelChar(active.ell), level,
                      dissectPlanCount);
        auto title = [&](uint16_t* fb, int x, int y, uint16_t color) {
            drawDissectTitle(fb, x, y, color, bigLabel, caption);
        };

        ESP_LOGI(kAtomViewTag, "dissecting shell %d%c (%d pts, level %d/%d)", active.n, subshellLabelChar(active.ell),
                 count, level, dissectPlanCount);

        flyOver(display, preset.points, preset.colors, count, title, protonColor, textColor, scaleBarColor, &camera,
                scale, s.baseScale, kSwitchTransitionFrames);
        scale = s.baseScale;

        int64_t holdStartUs = esp_timer_get_time();
        while (esp_timer_get_time() - holdStartUs < kDissectHoldUs) {
            renderDissectFrame(display, preset.points, preset.colors, count, protonColor, textColor, scaleBarColor,
                                camera, scale, bigLabel, caption);
            stepCamera(&camera);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Rebuild the full cloud fresh (undoes every level's in-place compaction above) and
    // ease back out to it.
    preset.load(preset.z);
    auto fullTitle = [&](uint16_t* fb, int x, int y, uint16_t color) {
        drawAtomTitle(fb, x, y, preset.z, preset.config, color, kFontLarge);
    };
    flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, fullTitle, protonColor, textColor,
            scaleBarColor, &camera, scale, preset.baseScale, kSwitchTransitionFrames);
}
} // namespace

void runAtomView(Display& display, TiltGestureDetector& tilt) {
    ESP_LOGI(kAtomViewTag, "display ready, Z=1..%d available", kMaxZ);

    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM: see orbital_view.cpp's sibling `preset`
    // for the full story (this struct, ~66KB+ of points+colors, is the bigger half of why
    // Display::Display()'s DMA frame-buffer allocation aborted at boot on real hardware).
    static EXT_RAM_BSS_ATTR AtomPresetState preset;
    if (preset.z == 0) // first-ever call this boot -- later calls (after a menu round-trip)
        preset.load(kAtomViewDefaultZ); // keep whatever element was last showing
    refreshDissectPlan(preset);

    constexpr uint16_t kProtonColor = Display::packColor565(255, 0, 0);
    constexpr uint16_t kTextColor = Display::kColorWhite;
    constexpr uint16_t kScaleBarColor = Display::kColorWhite;

    // preset has static storage duration, so it's odr-usable without capturing -- see
    // orbital_view.cpp's drawTitle for the same pattern.
    auto drawTitle = [](uint16_t* frameBuf, int x, int y, uint16_t color) {
        drawAtomTitle(frameBuf, x, y, preset.z, preset.config, color, kFontLarge);
    };

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
            kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames);

    constexpr int kFpsUpdateInterval = 50;
    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();
    int zoomExcursionCountdown = nextZoomExcursionCountdown();

    while (true) {
        TiltEvent tiltEv = tilt.poll();
        if (tiltEv.phase == TiltPhase::kConfirmed) {
            if (tiltEv.direction == TiltDirection::kUp) {
                ESP_LOGI(kAtomViewTag, "tilt UP confirmed -- returning to menu");
                return;
            }
            if (tiltEv.direction == TiltDirection::kRight || tiltEv.direction == TiltDirection::kLeft) {
                int delta = tiltEv.direction == TiltDirection::kRight ? 1 : -1;
                int newZ = preset.z + delta;
                if (newZ < 1)
                    newZ = kMaxZ;
                else if (newZ > kMaxZ)
                    newZ = 1;
                ESP_LOGI(kAtomViewTag, "tilt %s confirmed -- switching element Z %d -> %d (%s)",
                         tiltDirectionName(tiltEv.direction), preset.z, newZ, elementNameIt(newZ));
                orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
                scrollElementIntro(display, elementNameIt(newZ), newZ, elementSymbol(newZ), kAccentColor);
                preset.load(newZ);
                refreshDissectPlan(preset);
                flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor,
                        kTextColor, kScaleBarColor, &camera, currentScale, preset.baseScale, kSwitchTransitionFrames);
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                continue;
            }
            if (tiltEv.direction == TiltDirection::kDown) {
                if (dissectPlanCount > 0) {
                    ESP_LOGI(kAtomViewTag, "tilt DOWN confirmed -- starting automatic dissection (%d shells)",
                             dissectPlanCount);
                    runDissectionSequence(display, preset, camera, kProtonColor, kTextColor, kScaleBarColor);
                } else {
                    ESP_LOGI(kAtomViewTag, "tilt DOWN confirmed -- no subshells to dissect");
                }
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                continue;
            }
        }

        // Random zoom excursion: pause breathing, fly to a random scale and back, same as
        // orbital_view.cpp's steady-state loop. This iteration's render already happened
        // inside flyOver(), so skip the normal render/FPS bookkeeping below.
        zoomExcursionCountdown--;
        if (zoomExcursionCountdown <= 0) {
            orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
            orb_real_t targetScale =
                preset.baseScale * randomUniform(kZoomExcursionScaleMinFactor, kZoomExcursionScaleMaxFactor);
            flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
                    kScaleBarColor, &camera, currentScale, targetScale, kZoomExcursionEaseFrames);
            flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
                    kScaleBarColor, &camera, targetScale, preset.baseScale, kZoomExcursionEaseFrames);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
            continue;
        }

        orb_real_t scale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        renderScene(display.getFrameBuf(), preset.points, preset.colors, kAtomViewNumPoints, kProtonColor, camera,
                    scale);
        drawAtomTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, preset.z, preset.config, kTextColor,
                      kFontLarge);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
        if (tiltEv.phase != TiltPhase::kIdle)
            drawTiltArrow(display.getFrameBuf(), tiltEv.direction, kAccentColor);
        display.presentFrame();

        frameCount++;
        if (frameCount >= kFpsUpdateInterval) {
            int64_t nowUs = esp_timer_get_time();
            double elapsedS = double(nowUs - fpsWindowStartUs) / 1e6;
            double fps = elapsedS > 0 ? double(frameCount) / elapsedS : 0.0;
            ESP_LOGI(kAtomViewTag, "FPS: %.1f", fps);
            fpsWindowStartUs = nowUs;
            frameCount = 0;
        }

        stepCamera(&camera);
        zoomAngle += kZoomAngleStep;
        if (zoomAngle >= kTwoPi)
            zoomAngle -= kTwoPi;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
