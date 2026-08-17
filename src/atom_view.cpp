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
#include "periodic_grid.h"
#include "slater.h"
#include "ticker.h"

static const char* kAtomViewTag = "atom_view";

// Shared "fun accent" color for the two scrolling tickers below (element-switch Italian
// name, dissection-intro sentence) and the dissection view's big shell notation --
// matches tilt_gesture.h's arrow color, tying this project's few highlight-colored UI
// moments together instead of inventing a new one per feature.
constexpr uint16_t kAccentColor = Display::packColor565(255, 210, 60);

// "if no activity, element should change every 30s [later: 1 minute] with an animation"
// (feedback, 2026-08-17) -- idle threshold for AtomView's random auto-advance, see
// runAtomView()'s lastActivityUs tracking below. Shared shape with orbital_view.cpp's own
// idle timer, kept as separate constants per file rather than a shared header since nothing
// else needs them.
constexpr int64_t kIdleJumpUs = 60'000'000;

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

// "Make the element symbol bigger" (feedback, 2026-08-17) -- kFontLarge is already this
// project's biggest baked font, so drawAtomTitle() goes through font.h's drawTextScaled()
// instead, same trick as atom_view.h's kDissectBigScale/scrollElementIntro().
constexpr int kAtomTitleSymbolScale = 3;

void drawAtomTitle(uint16_t* frameBuf, int x, int y, int z, uint16_t textColor, const Font& font) {
    drawTextScaled(frameBuf, x, y, elementSymbol(z), textColor, font, kAtomTitleSymbolScale);
    char zLabel[16];
    std::snprintf(zLabel, sizeof(zLabel), "Z=%d", z);
    drawText(frameBuf, x, y + font.height * kAtomTitleSymbolScale + 4, zLabel, textColor, font);
}

// --- Element-switch intro ticker (Right/Left tilt-hold) -------------------------------

namespace {
constexpr int kElementIntroMaxNameScale = 4; // "bigger" (feedback, 2026-08-17) -- the biggest
constexpr int kElementIntroMinNameScale = 2; // that still fits is picked per-name, see pickNameScale()
constexpr int kElementIntroNameMarginPx = 10; // side margin so a centered name isn't flush to the edges
constexpr int kElementIntroSymbolScale = 6;   // big, static, pale watermark behind the scrolling name
// Light/muted on purpose -- it's a background, not the star of the animation; the
// scrolling name (kAccentColor) needs to stay the eye's focus on top of it.
constexpr uint16_t kElementIntroSymbolColor = Display::packColor565(90, 90, 100);

// Feedback (2026-08-17): the original single-pass scroll at ticker.h's default speed read
// as "too fast" -- slower here, plus a real pause instead of a continuous scroll-through.
constexpr int kElementIntroPxPerFrame = 6;
constexpr uint32_t kElementIntroHoldMs = 500;

/** Largest scale in [kElementIntroMinNameScale, kElementIntroMaxNameScale] whose scaled
 * width still leaves kElementIntroNameMarginPx clear on each side when centered -- longer
 * Italian names (e.g. "Praseodimio") don't fit as big as short ones (e.g. "Boro") without
 * clipping at the pause, so this is picked per-name rather than a single fixed scale. */
int pickNameScale(const char* name) {
    int maxWidth = Display::kDisplayWidth - 2 * kElementIntroNameMarginPx;
    for (int scale = kElementIntroMaxNameScale; scale > kElementIntroMinNameScale; scale--)
        if (textWidthScaled(name, kFontLarge, scale) <= maxWidth)
            return scale;
    return kElementIntroMinNameScale;
}

/**
 * Slide `nameIt` (the element's Italian name) in from the right over a big, static, pale
 * background watermark of `symbol` (its chemical symbol), pause centered for
 * kElementIntroHoldMs, then slide back out to the left -- shown before atom_view.cpp
 * switches to a new element. A static "Z=<z>" caption sits underneath the name throughout.
 * Bespoke (not ticker.h's plain scrollTextOnce()) since it composites three layers and a
 * pause instead of one plain continuous scroll.
 */
void scrollElementIntro(Display& display, const char* nameIt, int z, const char* symbol, uint16_t nameColor) {
    int nameScale = pickNameScale(nameIt);
    char zLabel[16];
    std::snprintf(zLabel, sizeof(zLabel), "Z=%d", z);

    int symbolWidth = textWidthScaled(symbol, kFontLarge, kElementIntroSymbolScale);
    int symbolX = (Display::kDisplayWidth - symbolWidth) / 2;
    int symbolY = (Display::kDisplayHeight - kFontLarge.height * kElementIntroSymbolScale) / 2;

    int nameY = 90;
    int zY = nameY + kFontLarge.height * nameScale + 6;
    int zX = (Display::kDisplayWidth - textWidth(zLabel, kFontLarge)) / 2;

    int nameWidth = textWidthScaled(nameIt, kFontLarge, nameScale);
    int centerX = (Display::kDisplayWidth - nameWidth) / 2;

    auto renderAt = [&](int x) {
        display.waitForFlushDone();
        uint16_t* frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
        drawTextScaled(frameBuf, symbolX, symbolY, symbol, kElementIntroSymbolColor, kFontLarge,
                        kElementIntroSymbolScale);
        drawTextScaled(frameBuf, x, nameY, nameIt, nameColor, kFontLarge, nameScale);
        drawText(frameBuf, zX, zY, zLabel, nameColor, kFontLarge);
        display.presentFrame();
    };

    for (int x = Display::kDisplayWidth; x > centerX; x -= kElementIntroPxPerFrame)
        renderAt(x);

    renderAt(centerX); // land exactly centered -- the loop above may step past it
    vTaskDelay(pdMS_TO_TICKS(kElementIntroHoldMs)); // panel keeps showing the last frame on its own

    for (int x = centerX; x > -nameWidth; x -= kElementIntroPxPerFrame)
        renderAt(x);
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

// "include the number of electrons next to the shell name: 1/2/3... e- (minus superscript)"
// (feedback, 2026-08-17) -- the minus is drawn as its own smaller drawCharScaled() call
// right after "Ne", no new font glyph needed (see font.h's drawCharScaled()).
constexpr int kDissectOccSuperscriptScale = kDissectBigScale > 1 ? kDissectBigScale - 1 : 1;

/** Draw `bigLabel` scaled kDissectBigScale x, then " Ne-" (electron count, minus as a
 * smaller superscript) right after it on the same line, then `caption` underneath at plain
 * size -- shared by the eased leg's easeScaleTimed() title callback and
 * renderDissectFrame()'s real-time hold below, so both look identical. */
void drawDissectTitle(uint16_t* frameBuf, int x, int y, uint16_t color, const char* bigLabel, const char* caption,
                       int occ) {
    int cursorX = drawTextScaled(frameBuf, x, y, bigLabel, color, kFontLarge, kDissectBigScale);
    char occText[8];
    std::snprintf(occText, sizeof(occText), " %de", occ);
    cursorX = drawTextScaled(frameBuf, cursorX, y, occText, color, kFontLarge, kDissectBigScale);
    drawCharScaled(frameBuf, cursorX, y, '-', color, kFontLarge, kDissectOccSuperscriptScale);
    drawText(frameBuf, x, y + kFontLarge.height * kDissectBigScale + 4, caption, color, kFontLarge);
}

/** Render one frame at a fixed `scale` -- shared by the eased leg (via easeScaleTimed())
 * and the real-time hold below, which holds a constant scale so doesn't need easing. */
void renderDissectFrame(Display& display, const AtomPoint* points, const uint16_t* colors, int count,
                         uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, const CameraState& camera,
                         orb_real_t scale, const char* bigLabel, const char* caption, int occ) {
    display.waitForFlushDone();
    renderScene(display.getFrameBuf(), points, colors, count, protonColor, camera, scale);
    drawDissectTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor, bigLabel, caption, occ);
    drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
    display.presentFrame();
}

// Feedback (2026-08-17): camera.h's flyOver() eases over a FIXED FRAME COUNT
// (kSwitchTransitionFrames), which reads as a different real-world speed depending on the
// achieved FPS -- this project's own perf work (CLAUDE.md's "35.7 -> 62.5 FPS" commit)
// sped it up enough that dissection's shell-to-shell zooms started feeling rushed instead
// of like "moving inside the atom". easeScaleTimed() below paces by real elapsed time
// (esp_timer) instead, at a fixed physical speed (kDissectFlySpeedPmPerSec) over the
// ACTUAL radial distance between the two shells' reference radii -- a hop between two
// closely-spaced shells is quick, a hop across a big radius gap takes longer, matching how
// flying past physical distance would feel, and staying independent of render FPS.
// "still too fast" (feedback, 2026-08-17, after an earlier 150 pm/s pass already tuned
// down once) -- more than halved again.
// "no no I want everything to be slower, no need for fancy math" (feedback, 2026-08-17) --
// plain further cut to the linear pm/s speed (was 60, halved again) instead of a non-linear
// reshaping; every hop just takes proportionally longer, big and small alike.
constexpr orb_real_t kDissectFlySpeedPmPerSec = orb_real_t(30);
// Floor so a ~zero-distance hop (e.g. entering dissection at level 1, already framed by
// the full-atom view -- see runDissectionSequence()'s `prevRRef` seeding) still eases
// briefly instead of cutting instantly. Raised alongside the speed cut above, same reason.
constexpr uint32_t kDissectFlyMinMs = 700;

/** Real-time duration (ms) to fly between two shells' reference radii at
 * kDissectFlySpeedPmPerSec, floored at kDissectFlyMinMs -- see the comment above. */
uint32_t dissectFlyDurationMs(orb_real_t fromRRef, orb_real_t toRRef) {
    orb_real_t deltaRRef = toRRef - fromRRef;
    orb_real_t distancePm = (deltaRRef < orb_real_t(0) ? -deltaRRef : deltaRRef) * kPmPerBohr;
    uint32_t ms = uint32_t(double(distancePm / kDissectFlySpeedPmPerSec) * 1000.0);
    return ms < kDissectFlyMinMs ? kDissectFlyMinMs : ms;
}

/**
 * Like camera.h's flyOver(), but eases `startScale` -> `endScale` over `durationMs` of
 * REAL time (esp_timer) instead of a fixed frame count -- see kDissectFlySpeedPmPerSec's
 * comment above for why. Template (over the title-drawing callable) for the same reason
 * flyOver() is: needs to be visible at each call site, but since every call site is in
 * this one file, it stays .cpp-local instead of needing a header (unlike flyOver(), which
 * orbital_view.cpp/atom_view.cpp both instantiate).
 */
template <typename TitleDrawFn>
void easeScaleTimed(Display& display, const AtomPoint* points, const uint16_t* colors, int count,
                     TitleDrawFn drawTitle, uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor,
                     CameraState& camera, orb_real_t startScale, orb_real_t endScale, uint32_t durationMs) {
    int64_t startUs = esp_timer_get_time();
    int64_t durationUs = int64_t(durationMs) * 1000;
    for (;;) {
        int64_t elapsedUs = esp_timer_get_time() - startUs;
        orb_real_t t = durationUs > 0 ? orb_real_t(double(elapsedUs) / double(durationUs)) : orb_real_t(1);
        if (t > orb_real_t(1))
            t = orb_real_t(1);
        orb_real_t scale = startScale + (endScale - startScale) * t;

        display.waitForFlushDone();
        renderScene(display.getFrameBuf(), points, colors, count, protonColor, camera, scale);
        drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
        display.presentFrame();

        stepCamera(&camera);
        if (t >= orb_real_t(1))
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * Automatically peel through every occupied subshell, outer to inner: for each level, ease
 * the camera in (easeScaleTimed(), paced by real time/pm-per-second -- see its comment) to
 * frame that subshell (scaleForAtom(active.rRef)) with its label, hold for kDissectHoldUs
 * while continuing to tumble, then move to the next level. Ends by rebuilding the full
 * cloud fresh (preset.load()) and easing back out to preset.baseScale. Blocking -- no tilt
 * polling during the sequence, matching pc/atom_view_pc.py's own one-shot blocking
 * sequence (no abort gesture defined here).
 */
// "same for dissection ... small pause, large characters" (feedback, 2026-08-17) --
// matches scrollElementIntro()'s slide-in/pause/slide-out shape, just without its
// background-watermark/Z-caption layers (this is one sentence, not a single word). Shorter
// hold than kElementIntroHoldMs: this phrase is long enough it's read partly in motion
// (see ticker.h's scrollTextPauseOnce() docstring on "centered" for text wider than the
// screen), not meant to fully stop and be absorbed like the short element name is.
constexpr int kDissectIntroScale = 2;
constexpr uint32_t kDissectIntroHoldMs = 400;

void runDissectionSequence(Display& display, AtomPresetState& preset, CameraState& camera, uint16_t protonColor,
                            uint16_t textColor, uint16_t scaleBarColor) {
    char introText[48];
    std::snprintf(introText, sizeof(introText), "Configurazione elettronica di %s", elementSymbol(preset.z));
    scrollTextPauseOnce(display, introText, kFontLarge, kDissectIntroScale, kAccentColor, 110, kDissectIntroHoldMs);

    orb_real_t scale = preset.baseScale;
    int count = kAtomViewNumPoints;
    // Seeded to dissectPlan[0]'s own radius (the outermost shell) -- numerically the same
    // reference outerSubshellRRef() used for preset.baseScale, so the very first hop
    // (level 1, already framed by the full-atom view) computes a ~zero distance and just
    // hits dissectFlyDurationMs()'s floor, not a spurious "long flight" to somewhere we're
    // already looking at.
    orb_real_t prevRRef = dissectPlanCount > 0 ? dissectPlan[0].rRef : orb_real_t(1);

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
            drawDissectTitle(fb, x, y, color, bigLabel, caption, active.occ);
        };

        uint32_t flyMs = dissectFlyDurationMs(prevRRef, active.rRef);
        ESP_LOGI(kAtomViewTag, "dissecting shell %d%c (%d pts, level %d/%d, fly %ums)", active.n,
                 subshellLabelChar(active.ell), count, level, dissectPlanCount, flyMs);

        easeScaleTimed(display, preset.points, preset.colors, count, title, protonColor, textColor, scaleBarColor,
                       camera, scale, s.baseScale, flyMs);
        scale = s.baseScale;
        prevRRef = active.rRef;

        int64_t holdStartUs = esp_timer_get_time();
        while (esp_timer_get_time() - holdStartUs < kDissectHoldUs) {
            renderDissectFrame(display, preset.points, preset.colors, count, protonColor, textColor, scaleBarColor,
                                camera, scale, bigLabel, caption, active.occ);
            stepCamera(&camera);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // Rebuild the full cloud fresh (undoes every level's in-place compaction above) and
    // ease back out to it, at the same fixed pm/s pace (a single hop covering the full
    // innermost-to-outermost distance, since we're returning straight to the full view).
    uint32_t returnFlyMs = dissectFlyDurationMs(prevRRef, dissectPlanCount > 0 ? dissectPlan[0].rRef : prevRRef);
    preset.load(preset.z);
    auto fullTitle = [&](uint16_t* fb, int x, int y, uint16_t color) {
        drawAtomTitle(fb, x, y, preset.z, color, kFontLarge);
    };
    easeScaleTimed(display, preset.points, preset.colors, kAtomViewNumPoints, fullTitle, protonColor, textColor,
                   scaleBarColor, camera, scale, preset.baseScale, returnFlyMs);
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
        drawAtomTitle(frameBuf, x, y, preset.z, color, kFontLarge);
    };

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
            kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames);

    constexpr int kFpsUpdateInterval = 50;
    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();
    int zoomExcursionCountdown = nextZoomExcursionCountdown();
    int64_t lastActivityUs = esp_timer_get_time();

    // Shared by every switch site below (Up/Down/Left/Right periodic-table movement, and
    // the idle random jump) -- was only written once (for Left/Right's plain Z+-1) before
    // periodic-table navigation gave every direction its own movement, and the idle timer
    // its own random target.
    auto switchToElement = [&](int newZ) {
        ESP_LOGI(kAtomViewTag, "switching element Z %d -> %d (%s)", preset.z, newZ, elementNameIt(newZ));
        orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        scrollElementIntro(display, elementNameIt(newZ), newZ, elementSymbol(newZ), kAccentColor);
        preset.load(newZ);
        refreshDissectPlan(preset);
        flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
                kScaleBarColor, &camera, currentScale, preset.baseScale, kSwitchTransitionFrames);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
    };

    while (true) {
        TiltEvent tiltEv = tilt.poll();
        if (tiltEv.phase == TiltPhase::kConfirmed || tiltEv.phase == TiltPhase::kConfirmedLong)
            lastActivityUs = esp_timer_get_time();

        if (tiltEv.phase == TiltPhase::kConfirmedLong) {
            if (tiltEv.direction == TiltDirection::kUp) {
                ESP_LOGI(kAtomViewTag, "tilt UP long-confirmed -- returning to menu");
                return;
            }
            if (tiltEv.direction == TiltDirection::kDown) {
                if (dissectPlanCount > 0) {
                    ESP_LOGI(kAtomViewTag, "tilt DOWN long-confirmed -- starting automatic dissection (%d shells)",
                             dissectPlanCount);
                    runDissectionSequence(display, preset, camera, kProtonColor, kTextColor, kScaleBarColor);
                } else {
                    ESP_LOGI(kAtomViewTag, "tilt DOWN long-confirmed -- no subshells to dissect");
                }
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                continue;
            }
        } else if (tiltEv.phase == TiltPhase::kConfirmed) {
            // Periodic-table 2D navigation (vertically, then horizontally -- see
            // periodic_grid.h): Up/Down move within the current element's column
            // (period), Left/Right move within its row (group), both wrapping at the
            // table's edges. Up/Down's plain-confirm actions (menu-return, dissection)
            // moved to a long hold instead -- see the kConfirmedLong branch above.
            int newZ = preset.z;
            bool moved = true;
            switch (tiltEv.direction) {
            case TiltDirection::kUp:
                newZ = periodicTableMoveVertical(preset.z, -1);
                break;
            case TiltDirection::kDown:
                newZ = periodicTableMoveVertical(preset.z, 1);
                break;
            case TiltDirection::kLeft:
                newZ = periodicTableMoveHorizontal(preset.z, -1);
                break;
            case TiltDirection::kRight:
                newZ = periodicTableMoveHorizontal(preset.z, 1);
                break;
            default:
                moved = false;
                break;
            }
            if (moved) {
                ESP_LOGI(kAtomViewTag, "tilt %s confirmed -- periodic-table move", tiltDirectionName(tiltEv.direction));
                switchToElement(newZ);
                continue;
            }
        }

        // Idle auto-advance: "just jump at random after 1 minute if no tilt has been
        // detected" (feedback, 2026-08-17) -- reuses switchToElement()'s exact animation,
        // just with a random target (randomIndexExcluding()) instead of a table-adjacent
        // one, so the transition looks identical whether user- or timer-triggered.
        if (esp_timer_get_time() - lastActivityUs > kIdleJumpUs) {
            int newZ = randomIndexExcluding(preset.z - 1, kMaxZ) + 1;
            ESP_LOGI(kAtomViewTag, "idle 60s+ -- jumping to random element Z=%d", newZ);
            switchToElement(newZ);
            lastActivityUs = esp_timer_get_time();
            continue;
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
        drawAtomTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, preset.z, kTextColor, kFontLarge);
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
