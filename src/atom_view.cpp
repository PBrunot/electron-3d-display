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

// "during random zoom it seems that the protons disappear" (feedback, 2026-08-17) -- same
// root cause already fixed in orbital_view.cpp: the shared flyOver()/renderScene() draw the
// nucleus BEFORE the cloud (matching pc/viewer_common.py's blend order on purpose, see
// camera.h), so a cloud point landing on the same pixel(s) alpha-blends over it and dims/
// hides it -- worst during a zoom excursion's extreme dive-in, where far points clip
// off-screen and only near-origin points remain, clustering densely right on top of the
// marker. Local bigger-marker + redraw-on-top fix, same shape as orbital_view.cpp's
// drawOrbitalProtonMarker()/orbitalFlyOver(); kept as atom_view.cpp's own copy rather than
// sharing code across the two .cpp files for a few lines.
constexpr int kAtomProtonMarkerSize = 7;

void drawAtomProtonMarker(uint16_t* frameBuf, uint16_t color) {
    int x0 = Display::kDisplayWidth / 2 - kAtomProtonMarkerSize / 2;
    int y0 = Display::kDisplayHeight / 2 - kAtomProtonMarkerSize / 2;
    for (int y = y0; y < y0 + kAtomProtonMarkerSize; y++)
        for (int x = x0; x < x0 + kAtomProtonMarkerSize; x++)
            frameBuf[y * Display::kDisplayWidth + x] = color;
}

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

// "element animation should conclude by flashing the element name in the background at
// 0.5Hz for 2s" (feedback, 2026-08-17) -- 0.5Hz is a 2s period (1s name-visible, 1s blank),
// so the requested 2s duration is exactly one full on/off cycle.
constexpr uint32_t kElementIntroFlashHalfPeriodMs = 1000;

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
 * kElementIntroHoldMs, then flash it on/off twice at 0.5Hz (2s total, see
 * kElementIntroFlashHalfPeriodMs) over that same background before returning -- shown before
 * atom_view.cpp switches to a new element. A static "Z=<z>" caption sits underneath the name
 * whenever it's visible. Bespoke (not ticker.h's plain scrollTextOnce()) since it composites
 * three layers, a pause, and a flash instead of one plain continuous scroll.
 */
void scrollElementIntro(Display& display, const char* nameIt, int z, const char* symbol, uint16_t nameColor) {
    int nameScale = pickNameScale(nameIt);
    char zLabel[16];
    std::snprintf(zLabel, sizeof(zLabel), "Z=%d", z);

    int symbolWidth = textWidthScaled(symbol, kFontLarge, kElementIntroSymbolScale);
    int symbolX = (Display::kDisplayWidth - symbolWidth) / 2;
    int symbolY = (Display::kDisplayHeight - kFontLarge.height * kElementIntroSymbolScale) / 2;

    // "element name in the 2/3 height, Z=xx in the upper 1/3 so that the element name
    // in background is clearly readable" (feedback, 2026-08-17) -- was name at y=90 with
    // the Z caption directly underneath; now the name sits at 2/3 of the panel height
    // (lower, clear of the centered symbol watermark) and the Z caption at 1/3 (upper),
    // so neither text crowds the other or the watermark.
    int nameY = Display::kDisplayHeight * 2 / 3;
    int zY = Display::kDisplayHeight / 3;
    int zX = (Display::kDisplayWidth - textWidth(zLabel, kFontLarge)) / 2;

    int nameWidth = textWidthScaled(nameIt, kFontLarge, nameScale);
    int centerX = (Display::kDisplayWidth - nameWidth) / 2;

    // `showName` false renders just the symbol watermark (the "background" the name flashes
    // over) with the name/Z-label blanked -- the flash tail below toggles this each half-period.
    auto renderAt = [&](int x, bool showName = true) {
        display.waitForFlushDone();
        uint16_t* frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
        drawTextScaled(frameBuf, symbolX, symbolY, symbol, kElementIntroSymbolColor, kFontLarge,
                        kElementIntroSymbolScale);
        if (showName) {
            drawTextScaled(frameBuf, x, nameY, nameIt, nameColor, kFontLarge, nameScale);
            drawText(frameBuf, zX, zY, zLabel, nameColor, kFontLarge);
        }
        display.presentFrame();
    };

    for (int x = Display::kDisplayWidth; x > centerX; x -= kElementIntroPxPerFrame)
        renderAt(x);

    renderAt(centerX); // land exactly centered -- the loop above may step past it
    vTaskDelay(pdMS_TO_TICKS(kElementIntroHoldMs)); // panel keeps showing the last frame on its own

    renderAt(centerX, false);
    vTaskDelay(pdMS_TO_TICKS(kElementIntroFlashHalfPeriodMs));
    renderAt(centerX, true);
    vTaskDelay(pdMS_TO_TICKS(kElementIntroFlashHalfPeriodMs));
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

// "the 2e- note should be somehow smaller and in the upper right corner to distinguish from
// the orbital name" (feedback, 2026-08-17) -- was inline right after bigLabel at the same
// big scale, easy to misread as part of the shell label itself; now a small standalone
// corner note (kFontSmall -- this project's designated secondary/readout size, see font.h)
// instead of a scaled-down bigLabel-style superscript.
constexpr int kDissectOccMarginPx = 4;

/** Draw `bigLabel` scaled kDissectBigScale x, `caption` underneath it at plain size, and the
 * electron count ("<occ>e-") as a small note in the top-right corner -- shared by the eased
 * leg's easeScaleTimed() title callback and renderDissectFrame()'s real-time hold below, so
 * both look identical. */
void drawDissectTitle(uint16_t* frameBuf, int x, int y, uint16_t color, const char* bigLabel, const char* caption,
                       int occ) {
    drawTextScaled(frameBuf, x, y, bigLabel, color, kFontLarge, kDissectBigScale);
    drawText(frameBuf, x, y + kFontLarge.height * kDissectBigScale + 4, caption, color, kFontLarge);

    char occText[8];
    std::snprintf(occText, sizeof(occText), "%de-", occ);
    int occX = Display::kDisplayWidth - textWidth(occText, kFontSmall) - kDissectOccMarginPx;
    drawText(frameBuf, occX, kDissectOccMarginPx, occText, color, kFontSmall);
}

/** Render one frame at a fixed `scale` -- shared by the eased leg (via easeScaleTimed())
 * and the real-time hold below, which holds a constant scale so doesn't need easing. */
void renderDissectFrame(Display& display, const AtomPoint* points, const uint16_t* colors, int count,
                         uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, const CameraState& camera,
                         orb_real_t scale, const char* bigLabel, const char* caption, int occ) {
    display.waitForFlushDone();
    renderScene(display.getFrameBuf(), points, colors, count, protonColor, camera, scale);
    drawAtomProtonMarker(display.getFrameBuf(), protonColor); // see its docstring -- keep it visible over the cloud
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
// "any movement during the dissection should close the dissection and bring back to the
// element" (feedback, 2026-08-17) -- `tilt`, if non-null, is polled every frame; any
// non-kIdle phase (the device starting to tip, not necessarily a full confirmed hold) aborts
// immediately and returns false instead of running to completion. Used by
// runDissectionSequence()'s per-level eases below; the default nullptr keeps this function's
// other caller (dissection's own final "back to full view" leg, which IS the closing action,
// not something to interrupt) unabortable.
template <typename TitleDrawFn>
bool easeScaleTimed(Display& display, const AtomPoint* points, const uint16_t* colors, int count,
                     TitleDrawFn drawTitle, uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor,
                     CameraState& camera, orb_real_t startScale, orb_real_t endScale, uint32_t durationMs,
                     TiltGestureDetector* tilt = nullptr) {
    int64_t startUs = esp_timer_get_time();
    int64_t durationUs = int64_t(durationMs) * 1000;
    for (;;) {
        if (tilt && tilt->poll().phase != TiltPhase::kIdle)
            return false;

        int64_t elapsedUs = esp_timer_get_time() - startUs;
        orb_real_t t = durationUs > 0 ? orb_real_t(double(elapsedUs) / double(durationUs)) : orb_real_t(1);
        if (t > orb_real_t(1))
            t = orb_real_t(1);
        orb_real_t scale = startScale + (endScale - startScale) * t;

        display.waitForFlushDone();
        renderScene(display.getFrameBuf(), points, colors, count, protonColor, camera, scale);
        drawAtomProtonMarker(display.getFrameBuf(), protonColor); // see its docstring -- keep it visible over the cloud
        drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
        display.presentFrame();

        stepCamera(&camera);
        if (t >= orb_real_t(1))
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

/**
 * Automatically peel through every occupied subshell, outer to inner: for each level, ease
 * the camera in (easeScaleTimed(), paced by real time/pm-per-second -- see its comment) to
 * frame that subshell (scaleForAtom(active.rRef)) with its label, hold for kDissectHoldUs
 * while continuing to tumble, then move to the next level. Ends by rebuilding the full
 * cloud fresh (preset.load()) and easing back out to preset.baseScale.
 *
 * "any movement during the dissection should close the dissection and bring back to the
 * element" (feedback, 2026-08-17) -- `tilt` is polled every eased frame and every hold
 * frame; any movement (even a not-yet-confirmed tilt candidate) breaks out of the level loop
 * early and falls straight through to the same rebuild-and-return-to-full-view tail every
 * completed run also ends with, so an interrupted dissection lands back on the full element
 * exactly like a finished one does.
 */
// "should be configurazione\nelettronica\n<nome elemento in italiano> -- nice to have a
// background with e-" (feedback, 2026-08-17) -- replaces the old single-line scrolling
// sentence with a static 3-line title card (the two fixed Italian words don't need scrolling
// to be read, and the element name is usually short enough not to either -- see
// showElectronConfigIntro()'s nameScale fallback for the rare long name that doesn't fit),
// over a tiled dim "e-" backdrop evoking the electrons this sequence is about to walk through.
constexpr int kDissectIntroWordScale = 2; // "Configurazione" / "elettronica"
constexpr int kDissectIntroLineGapPx = 50; // successive line start-to-start, scale2-height (40) + 10
constexpr uint32_t kDissectIntroHoldMs = 900; // was 400 -- no scroll motion to read during anymore
constexpr uint16_t kDissectIntroBgColor = Display::packColor565(55, 55, 55); // dim -- backdrop, not the focus
constexpr int kDissectIntroBgSpacingX = 44, kDissectIntroBgSpacingY = 34;

/** Tile "e-" across the whole frame in a dim, unobtrusive grid -- drawn first so the title
 * lines composite on top of it (plain overwrite, no blending, matching every other draw
 * function in this project). */
void drawElectronBackdrop(uint16_t* frameBuf) {
    for (int y = 6; y < Display::kDisplayHeight; y += kDissectIntroBgSpacingY)
        for (int x = 6; x < Display::kDisplayWidth; x += kDissectIntroBgSpacingX)
            drawText(frameBuf, x, y, "e-", kDissectIntroBgColor, kFontSmall);
}

/** Static 3-line "Configurazione / elettronica / <nameIt>" title card over the electron
 * backdrop, held for kDissectIntroHoldMs before the dissection sequence itself starts. */
void showElectronConfigIntro(Display& display, const char* nameIt) {
    constexpr const char* kLine1 = "Configurazione";
    constexpr const char* kLine2 = "elettronica";

    // Same size as the two fixed words unless the element name is too wide for it (long
    // Italian names, e.g. "Praseodimio") -- consistent size across all 3 lines is the
    // common case, not scrollElementIntro()'s per-name-length pickNameScale() (that one
    // sizes a name to fill the whole screen alone; here it shares the screen with two other
    // lines, so it should match them, not dominate).
    int nameScale = kDissectIntroWordScale;
    if (textWidthScaled(nameIt, kFontLarge, nameScale) > Display::kDisplayWidth - 20)
        nameScale = 1;

    int y1 = 50, y2 = y1 + kDissectIntroLineGapPx, y3 = y2 + kDissectIntroLineGapPx;
    int x1 = (Display::kDisplayWidth - textWidthScaled(kLine1, kFontLarge, kDissectIntroWordScale)) / 2;
    int x2 = (Display::kDisplayWidth - textWidthScaled(kLine2, kFontLarge, kDissectIntroWordScale)) / 2;
    int x3 = (Display::kDisplayWidth - textWidthScaled(nameIt, kFontLarge, nameScale)) / 2;

    display.waitForFlushDone();
    uint16_t* frameBuf = display.getFrameBuf();
    std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
    drawElectronBackdrop(frameBuf);
    drawTextScaled(frameBuf, x1, y1, kLine1, kAccentColor, kFontLarge, kDissectIntroWordScale);
    drawTextScaled(frameBuf, x2, y2, kLine2, kAccentColor, kFontLarge, kDissectIntroWordScale);
    drawTextScaled(frameBuf, x3, y3, nameIt, kAccentColor, kFontLarge, nameScale);
    display.presentFrame();

    vTaskDelay(pdMS_TO_TICKS(kDissectIntroHoldMs));
}

void runDissectionSequence(Display& display, AtomPresetState& preset, CameraState& camera, uint16_t protonColor,
                            uint16_t textColor, uint16_t scaleBarColor, TiltGestureDetector& tilt) {
    showElectronConfigIntro(display, elementNameIt(preset.z));

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

        bool completed = easeScaleTimed(display, preset.points, preset.colors, count, title, protonColor, textColor,
                                         scaleBarColor, camera, scale, s.baseScale, flyMs, &tilt);
        scale = s.baseScale;
        prevRRef = active.rRef;
        if (!completed) {
            ESP_LOGI(kAtomViewTag, "dissection aborted -- movement detected mid-fly");
            break;
        }

        int64_t holdStartUs = esp_timer_get_time();
        bool aborted = false;
        while (esp_timer_get_time() - holdStartUs < kDissectHoldUs) {
            if (tilt.poll().phase != TiltPhase::kIdle) {
                ESP_LOGI(kAtomViewTag, "dissection aborted -- movement detected during hold");
                aborted = true;
                break;
            }
            renderDissectFrame(display, preset.points, preset.colors, count, protonColor, textColor, scaleBarColor,
                                camera, scale, bigLabel, caption, active.occ);
            stepCamera(&camera);
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (aborted)
            break;
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

/** Like camera.h's flyOver(), but redraws the proton marker on top of every frame after the
 * cloud -- see drawAtomProtonMarker()'s docstring above for why. Used for the boot intro,
 * element switches, and random zoom excursions; easeScaleTimed()/renderDissectFrame() above
 * get the same redraw inline since they're already local to this file. */
template <typename TitleDrawFn>
void atomFlyOver(Display& display, const AtomPoint* points, const uint16_t* colors, int count, TitleDrawFn drawTitle,
                  uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, CameraState* camera,
                  orb_real_t startScale, orb_real_t endScale, int frames) {
    for (int i = 0; i < frames; i++) {
        orb_real_t t = frames > 1 ? orb_real_t(i) / orb_real_t(frames - 1) : orb_real_t(1);
        orb_real_t scale = startScale + (endScale - startScale) * t;

        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        renderScene(display.getFrameBuf(), points, colors, count, protonColor, *camera, scale);
        drawAtomProtonMarker(display.getFrameBuf(), protonColor);
        drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
        display.presentFrame();

        stepCamera(camera);
    }
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

    atomFlyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
            kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames);

    constexpr int kFpsUpdateInterval = 50;
    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();
    int zoomExcursionCountdown = nextZoomExcursionCountdown();
    int64_t lastActivityUs = esp_timer_get_time();
    // "animation should decide whether to jump to next element or dissect this one (max.
    // once). After 60s it should jump to another element" (feedback, 2026-08-17) -- caps
    // the idle auto-advance to at most one dissection per element before it's forced to
    // jump, so idle browsing doesn't get stuck re-dissecting the same element every 60s.
    // Reset to false whenever a new element loads, see switchToElement() below.
    bool idleDissectedThisElement = false;

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
        idleDissectedThisElement = false; // fresh element -- fresh idle dissection budget
        atomFlyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
                kScaleBarColor, &camera, currentScale, preset.baseScale, kSwitchTransitionFrames);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
        // "why the FPS dips" (feedback, 2026-08-17) -- scrollElementIntro()'s real-time
        // holds plus this flyOver() spend real wall-clock time without incrementing
        // frameCount, so leaving the FPS window running let that time get charged to
        // whichever later window happened to cross the kFpsUpdateInterval frame count,
        // reporting a bogus low FPS for a window that was otherwise rendered at full speed.
        // Not an actual slowdown -- just a mismeasured one; reset the window here so it
        // only ever measures steady-state frames.
        frameCount = 0;
        fpsWindowStartUs = esp_timer_get_time();
    };

    while (true) {
        TiltEvent tiltEv = tilt.poll();
        if (tiltEv.phase == TiltPhase::kConfirmed)
            lastActivityUs = esp_timer_get_time();

        // "U/D tilts moving us in a specific order in the periodic table (vertical then
        // horizontal), L should bring back to the menu and R start dissection" (feedback,
        // 2026-08-17): a single axis (Up/Down) walks the WHOLE table in one order (see
        // periodicTableSnakeStep()'s doc comment), freeing Left/Right for menu-return/
        // dissection -- no long-hold tier needed.
        if (tiltEv.phase == TiltPhase::kConfirmed) {
            if (tiltEv.direction == TiltDirection::kUp || tiltEv.direction == TiltDirection::kDown) {
                int delta = tiltEv.direction == TiltDirection::kDown ? 1 : -1;
                int newZ = periodicTableSnakeStep(preset.z, delta);
                ESP_LOGI(kAtomViewTag, "tilt %s confirmed -- periodic-table move", tiltDirectionName(tiltEv.direction));
                switchToElement(newZ);
                continue;
            }
            if (tiltEv.direction == TiltDirection::kLeft) {
                ESP_LOGI(kAtomViewTag, "tilt LEFT confirmed -- returning to menu");
                return;
            }
            if (tiltEv.direction == TiltDirection::kRight) {
                if (dissectPlanCount > 0) {
                    ESP_LOGI(kAtomViewTag, "tilt RIGHT confirmed -- starting automatic dissection (%d shells)",
                             dissectPlanCount);
                    runDissectionSequence(display, preset, camera, kProtonColor, kTextColor, kScaleBarColor, tilt);
                } else {
                    ESP_LOGI(kAtomViewTag, "tilt RIGHT confirmed -- no subshells to dissect");
                }
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                // See switchToElement()'s comment above -- same unmeasured-time issue.
                frameCount = 0;
                fpsWindowStartUs = esp_timer_get_time();
                continue;
            }
        }

        // Idle auto-advance: "just jump at random after 1 minute if no tilt has been
        // detected" (feedback, 2026-08-17), refined by "animation should decide whether to
        // jump to next element or dissect this one (max. once). After 60s it should jump to
        // another element" (feedback, 2026-08-17) -- each idle timeout has a coin-flip
        // chance to dissect the CURRENT element instead of jumping, but only once per
        // element (idleDissectedThisElement, cleared on every switchToElement()) and only
        // when there's actually something to dissect; once that's used up (or there's
        // nothing to dissect), idle timeouts always jump, same as before.
        if (esp_timer_get_time() - lastActivityUs > kIdleJumpUs) {
            bool canDissect = !idleDissectedThisElement && dissectPlanCount > 0;
            if (canDissect && randomUnit() < orb_real_t(0.5)) {
                ESP_LOGI(kAtomViewTag, "idle 60s+ -- dissecting current element (Z=%d, %d shells)", preset.z,
                         dissectPlanCount);
                runDissectionSequence(display, preset, camera, kProtonColor, kTextColor, kScaleBarColor, tilt);
                idleDissectedThisElement = true;
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                // See switchToElement()'s FPS-window comment above -- same unmeasured-time issue.
                frameCount = 0;
                fpsWindowStartUs = esp_timer_get_time();
            } else {
                int newZ = randomIndexExcluding(preset.z - 1, kMaxZ) + 1;
                ESP_LOGI(kAtomViewTag, "idle 60s+ -- jumping to random element Z=%d", newZ);
                switchToElement(newZ);
            }
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
            atomFlyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
                    kScaleBarColor, &camera, currentScale, targetScale, kZoomExcursionEaseFrames);
            atomFlyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
                    kScaleBarColor, &camera, targetScale, preset.baseScale, kZoomExcursionEaseFrames);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
            // See switchToElement()'s comment above -- same unmeasured-time issue (this is
            // the specific case the log dips came from: the two 45-frame flyOver()s above
            // ran for real wall-clock time that the FPS window didn't know about).
            frameCount = 0;
            fpsWindowStartUs = esp_timer_get_time();
            continue;
        }

        orb_real_t scale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        renderScene(display.getFrameBuf(), preset.points, preset.colors, kAtomViewNumPoints, kProtonColor, camera,
                    scale);
        drawAtomProtonMarker(display.getFrameBuf(), kProtonColor); // see its docstring -- keep it visible over the cloud
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
