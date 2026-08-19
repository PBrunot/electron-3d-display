#include "views/atom_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ux/element_names_it.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_log.h"
#include "esp_timer.h"
#include "render/font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "render/overlay.h"
#include "ux/periodic_grid.h"
#include "debug/screenshot_pause.h"
#include "physics/slater.h"
#include "config/visual_constants.h" // kAccentColor, kViewIdleJumpUs, kAtomProtonMarkerSize, kElementIntro*, kDissect*, kFpsUpdateInterval

static const char *kAtomViewTag = "atom_view";

void drawAtomProtonMarker(uint16_t *frameBuf, uint16_t color)
{
    int x0 = Display::kDisplayWidth / 2 - kAtomProtonMarkerSize / 2;
    int y0 = Display::kDisplayHeight / 2 - kAtomProtonMarkerSize / 2;
    for (int y = y0; y < y0 + kAtomProtonMarkerSize; y++)
        for (int x = x0; x < x0 + kAtomProtonMarkerSize; x++)
            frameBuf[y * Display::kDisplayWidth + x] = color;
}

void AtomPresetState::load(int zIn)
{
    ESP_LOGI(kAtomViewTag, "loading Z=%d (%s)...", zIn, elementSymbol(zIn));
    int64_t startUs = esp_timer_get_time();

    config = buildAtomPointCloud(zIn, points, kAtomNumPoints, kAtomCloudSeed, ranges, &rangeCount);
    OuterSubshell outer = outerSubshellRRef(points, ranges, rangeCount);
    uint16_t subshellColors[kMaxConfigSubshells];
    colorizeAtomSubshells(ranges, rangeCount, outer, subshellColors);
    groupCount = rangeCount;
    for (int s = 0; s < rangeCount; s++)
        groups[s] = PointGroup{ranges[s].startIndex, ranges[s].count, subshellColors[s]};

    AtomScale scale = scaleForAtom(outer.rRef);
    baseScale = scale.baseScale;
    zoomAmplitude = scale.zoomAmplitude;
    z = zIn;

    int64_t buildMs = (esp_timer_get_time() - startUs) / 1000;
    ESP_LOGI(kAtomViewTag, "%s loaded in %lldms, outer=%d%c, outerRBohr=%.2f, scale=%.1f, outerRPx=%.1f",
             elementSymbol(zIn), buildMs, outer.n, subshellLabelChar(outer.ell), double(outer.rRef),
             double(baseScale), double(outer.rRef * baseScale));
}

void drawAtomTitle(uint16_t *frameBuf, int x, int y, int z, uint16_t textColor)
{
    drawText(frameBuf, x, y, elementSymbol(z), textColor, kFontHuge);
}

// --- Element-switch intro ticker (Right/Left tilt-hold) -------------------------------

namespace
{
    int pickNameScale(const char *name)
    {
        int maxWidth = Display::kDisplayWidth - 2 * kElementIntroNameMarginPx;
        for (int scale = kElementIntroMaxNameScale; scale > kElementIntroMinNameScale; scale--)
            if (textWidthScaled(name, kFontLarge, scale) <= maxWidth)
                return scale;
        return kElementIntroMinNameScale;
    }

    /**
     * @brief Intro shown before atom_view.cpp switches to a new element.
     *
     * Slides `nameIt` (the element's Italian name) in from the right over a static, pale
     * background watermark of `symbol`, pauses centered, then flashes it on/off twice before
     * returning. A static "Z=<z>" caption sits underneath the name whenever it's visible; the
     * name sits at 2/3 panel height and the caption at 1/3, keeping both clear of the centered
     * watermark. Bespoke rather than ticker.h's plain scrollTextOnce() since it composites
     * three layers, a pause, and a flash instead of one continuous scroll.
     */
    void scrollElementIntro(Display &display, const char *nameIt, int z, const char *symbol, uint16_t nameColor)
    {
        int nameScale = pickNameScale(nameIt);
        char zLabel[16];
        std::snprintf(zLabel, sizeof(zLabel), "Z = %d", z);

        int symbolWidth = textWidthScaled(symbol, kFontHuge, kElementIntroSymbolScale);
        int symbolX = (Display::kDisplayWidth - symbolWidth) / 2;
        int symbolY = (Display::kDisplayHeight - kFontHuge.height * kElementIntroSymbolScale) / 2;

        int nameY = Display::kDisplayHeight * 2 / 3;
        int zY = Display::kDisplayHeight / 4;
        int zX = (Display::kDisplayWidth - textWidth(zLabel, kFontHuge)) / 2;

        int nameWidth = textWidthScaled(nameIt, kFontLarge, nameScale);
        int centerX = (Display::kDisplayWidth - nameWidth) / 2;

        // `showName` false renders just the symbol watermark (the "background" the name
        // flashes over); the flash tail below toggles this each half-period.
        auto renderAt = [&](int x, bool showName = true)
        {
            display.waitForFlushDone();
            uint16_t *frameBuf = display.getFrameBuf();
            std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
            drawTextScaled(frameBuf, symbolX, symbolY, symbol, kElementIntroSymbolColor, kFontHuge,
                           kElementIntroSymbolScale);
            if (showName)
            {
                drawTextScaled(frameBuf, x, nameY, nameIt, nameColor, kFontLarge, nameScale);
                drawText(frameBuf, zX, zY, zLabel, nameColor, kFontHuge);
            }
            display.presentFrame();
        };

        for (int x = Display::kDisplayWidth; x > centerX; x -= kElementIntroPxPerFrame)
            renderAt(x);

        renderAt(centerX);
        vTaskDelay(pdMS_TO_TICKS(kElementIntroHoldMs));

        // Accelerating blink
        for (int i = 1; i < 6; i++)
        {
            renderAt(centerX, false);
            vTaskDelay(pdMS_TO_TICKS(kElementIntroFlashHalfPeriodMs / i));
            renderAt(centerX, true);
            vTaskDelay(pdMS_TO_TICKS(kElementIntroFlashHalfPeriodMs / i));
        }
    }
} // namespace

// --- On-device shell dissection (Down tilt-hold) -- see atom_view.h's header comment ---
//
// Runs as one self-contained blocking sequence once triggered, not one dissect step per
// hold: a single Down-hold peels through every occupied subshell automatically (eased zoom +
// a real-time hold on each, continuously tumbling throughout) and returns to the full atom at
// the end, with no further gesture needed mid-sequence.

namespace
{
    DissectionEntry dissectPlan[kMaxConfigSubshells];
    int dissectPlanCount = 0;

    void refreshDissectPlan(const AtomPresetState &preset)
    {
        dissectPlanCount = subshellDissectionPlan(preset.points, preset.ranges, preset.rangeCount, dissectPlan);
    }

    /**
     * @brief Build the render groups visible at dissect level `level` (1..dissectPlanCount),
     *        without moving any point data.
     *
     * dissectPlan is sorted outermost-first (see subshellDissectionPlan()), and each entry
     * already carries its own contiguous point range -- so "peeling away" the subshells outer
     * of level `level` is just excluding their groups from the output, not compacting/copying
     * points[] (unlike this function's predecessor, compactDissectLevelInPlace(), which used to
     * physically shrink a copy of the cloud on every level change; see this file's prior
     * revision if that history is ever needed). dissectPlan[level-1] itself (the newly-revealed
     * outermost remaining shell) draws at full shell color; every deeper shell draws flat gray
     * (kDissectDimColor).
     *
     * @param outGroups  [out] Must hold at least kMaxConfigSubshells entries.
     * @return           Number of groups written (== dissectPlanCount - (level - 1)).
     */
    int buildDissectGroups(int level, PointGroup *outGroups)
    {
        int written = 0;
        for (int rank = level - 1; rank < dissectPlanCount; rank++)
        {
            const DissectionEntry &e = dissectPlan[rank];
            uint16_t color;
            if (rank == level - 1)
            {
                const uint8_t *base = shellBaseRgb(e.n);
                color = Display::packColor565(base[0], base[1], base[2]);
            }
            else
            {
                color = kDissectDimColor;
            }
            outGroups[written++] = PointGroup{e.startIndex, e.count, color};
        }
        return written;
    }

    /**
     * @brief Draw `bigLabel` scaled up, `caption` underneath at plain size, and the electron
     *        count ("<occ>e-") as a small note in the top-right corner.
     *
     * Shared by the eased leg's title callback and renderDissectFrame()'s real-time hold, so
     * both look identical. The occupancy note stays small and corner-anchored so it isn't
     * mistaken for part of the big shell-notation label.
     */
    void drawDissectTitle(uint16_t *frameBuf, int x, int y, uint16_t color, const char *bigLabel, const char *caption,
                          int occ)
    {
        // kFontHuge at its own true size, not kFontLarge integer-upscaled -- its design
        // size (54px) happens to land almost exactly on kFontLarge's old x3 scale, so this
        // swap keeps the label the same visual size while replacing blocky tripled pixels
        // with real glyph shapes (same fix kFontHuge exists for on the element-symbol title).
        drawText(frameBuf, x, y, bigLabel, color, kFontHuge);
        drawText(frameBuf, x, y + kFontHuge.height + 4, caption, color, kFontLarge);

        char occText[8];
        std::snprintf(occText, sizeof(occText), "%de-", occ);
        int occX = Display::kDisplayWidth - textWidth(occText, kFontHuge) - kDissectOccMarginPx;
        drawText(frameBuf, occX, kDissectOccMarginPx, occText, Display::kColorOrbitalBlue, kFontHuge);
    }

    /// Render one frame at a fixed `scale`, for the real-time hold (which doesn't need
    /// easing). `frameSalt`/`buzzThreshold` feed renderScene()'s hidden-points buzz (see
    /// camera.h's kHiddenPointsFraction); the caller owns the per-call frameSalt counter.
    void renderDissectFrame(Display &display, const AtomPoint *points, const PointGroup *groups, int groupCount,
                            uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, const CameraState &camera,
                            orb_real_t scale, const char *bigLabel, const char *caption, int occ,
                            uint32_t frameSalt = 0, uint32_t buzzThreshold = 0)
    {
        display.waitForFlushDone();
        renderSceneGrouped(display.getFrameBuf(), points, groups, groupCount, protonColor, camera, scale, frameSalt,
                           buzzThreshold);
        drawAtomProtonMarker(display.getFrameBuf(), protonColor);
        drawDissectTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor, bigLabel, caption, occ);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
        display.presentFrame();
    }

    /// Real-time duration (ms) to fly between two shells' reference radii at
    /// kDissectFlySpeedPmPerSec, floored at kDissectFlyMinMs.
    uint32_t dissectFlyDurationMs(orb_real_t fromRRef, orb_real_t toRRef)
    {
        orb_real_t deltaRRef = toRRef - fromRRef;
        orb_real_t distancePm = (deltaRRef < orb_real_t(0) ? -deltaRRef : deltaRRef) * kPmPerBohr;
        uint32_t ms = uint32_t(double(distancePm / kDissectFlySpeedPmPerSec) * 1000.0);
        return ms < kDissectFlyMinMs ? kDissectFlyMinMs : ms;
    }

    /**
     * @brief Like camera.h's flyOver(), but eases `startScale` -> `endScale` over `durationMs`
     *        of real time (esp_timer) instead of a fixed frame count.
     *
     * Templated on the title-drawing callable for the same reason flyOver() is (needs to be
     * visible at each call site); kept .cpp-local since every call site is in this file.
     *
     * If `tilt` is non-null, it's polled every frame; any non-idle phase (the device starting
     * to tip, not necessarily a full confirmed hold) aborts immediately and returns false, so a
     * dissection in progress can be cancelled by movement. The default nullptr is used for the
     * sequence's own final "back to full view" leg, which is the closing action and should not
     * itself be interruptible.
     */
    template <typename TitleDrawFn>
    bool easeScaleTimed(Display &display, const AtomPoint *points, const PointGroup *groups, int groupCount,
                        TitleDrawFn drawTitle, uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor,
                        CameraState &camera, orb_real_t startScale, orb_real_t endScale, uint32_t durationMs,
                        TiltGestureDetector *tilt = nullptr, uint32_t buzzThreshold = 0)
    {
        int64_t startUs = esp_timer_get_time();
        int64_t durationUs = int64_t(durationMs) * 1000;
        uint32_t frameSalt = 0; // real-time loop, not a fixed frame count -- own counter, see camera.h's flyOver()
        for (;;)
        {
            if (tilt && tilt->poll().phase != TiltPhase::kIdle)
                return false;

            int64_t elapsedUs = esp_timer_get_time() - startUs;
            orb_real_t t = durationUs > 0 ? orb_real_t(double(elapsedUs) / double(durationUs)) : orb_real_t(1);
            if (t > orb_real_t(1))
                t = orb_real_t(1);
            orb_real_t scale = startScale + (endScale - startScale) * t;

            display.waitForFlushDone();
            renderSceneGrouped(display.getFrameBuf(), points, groups, groupCount, protonColor, camera, scale,
                               frameSalt, buzzThreshold);
            drawAtomProtonMarker(display.getFrameBuf(), protonColor); // keep it visible over the cloud
            drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
            drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
            display.presentFrame();

            stepCamera(&camera);
            frameSalt++;
            if (t >= orb_real_t(1))
                break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return true;
    }

    /// Tile "e-" across the whole frame in a dim, unobtrusive grid -- drawn first so the title
    /// lines composite on top of it (plain overwrite, no blending, matching every other draw
    /// function in this project).
    void drawElectronBackdrop(uint16_t *frameBuf)
    {
        for (int y = 6; y < Display::kDisplayHeight; y += kDissectIntroBgSpacingY)
            for (int x = 6; x < Display::kDisplayWidth; x += kDissectIntroBgSpacingX)
                drawText(frameBuf, x, y, "e-", kDissectIntroBgColor, kFontSmall);
    }

    /// Static 3-line "Configurazione / elettronica / <nameIt>" title card over the electron
    /// backdrop, held for kDissectIntroHoldMs before the dissection sequence itself starts.
    void showElectronConfigIntro(Display &display, const char *nameIt)
    {
        constexpr const char *kLine1 = "Configurazione";
        constexpr const char *kLine2 = "elettronica";

        // Falls back to scale 1 only if the element name is too wide at the shared word scale
        // (e.g. long Italian names like "Praseodimio") -- otherwise all 3 lines match in size.
        int nameScale = kDissectIntroWordScale;
        if (textWidthScaled(nameIt, kFontLarge, nameScale) > Display::kDisplayWidth - 20)
            nameScale = 1;

        int y1 = 50, y2 = y1 + kDissectIntroLineGapPx, y3 = y2 + kDissectIntroLineGapPx;
        int x1 = (Display::kDisplayWidth - textWidthScaled(kLine1, kFontLarge, kDissectIntroWordScale)) / 2;
        int x2 = (Display::kDisplayWidth - textWidthScaled(kLine2, kFontLarge, kDissectIntroWordScale)) / 2;
        int x3 = (Display::kDisplayWidth - textWidthScaled(nameIt, kFontLarge, nameScale)) / 2;

        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
        drawElectronBackdrop(frameBuf);
        drawTextScaled(frameBuf, x1, y1, kLine1, kAccentColor, kFontLarge, kDissectIntroWordScale);
        drawTextScaled(frameBuf, x2, y2, kLine2, kAccentColor, kFontLarge, kDissectIntroWordScale);
        drawTextScaled(frameBuf, x3, y3, nameIt, kAccentColor, kFontLarge, nameScale);
        display.presentFrame();

        vTaskDelay(pdMS_TO_TICKS(kDissectIntroHoldMs));
    }

    /**
     * @brief Automatically peel through every occupied subshell, outer to inner.
     *
     * For each level: ease the camera in (easeScaleTimed(), paced by real time) to frame that
     * subshell with its label, hold for kDissectHoldUs while continuing to tumble, then move to
     * the next level. `tilt` is polled every eased and held frame; any movement breaks out of
     * the level loop early. Either way (completed or interrupted) execution falls through to
     * the same tail: ease back out to preset.baseScale using preset's own unmodified render
     * groups. Unlike an earlier revision of this function, `preset.points`/`preset.groups` are
     * never touched during the loop -- buildDissectGroups() only ever reads dissectPlan's
     * point ranges into a level-local group list, so there's nothing to rebuild/undo here, and
     * an interrupted dissection lands back on the full element exactly like a finished one does.
     */
    void runDissectionSequence(Display &display, AtomPresetState &preset, CameraState &camera, uint16_t protonColor,
                               uint16_t textColor, uint16_t scaleBarColor, TiltGestureDetector &tilt)
    {
        showElectronConfigIntro(display, elementNameIt(preset.z));

        orb_real_t scale = preset.baseScale;
        // Seeded to dissectPlan[0]'s own radius (the outermost shell, numerically the same
        // reference used for preset.baseScale) so the very first hop -- level 1, already
        // framed by the full-atom view -- computes a near-zero distance and just hits
        // dissectFlyDurationMs()'s floor instead of a spurious long flight.
        orb_real_t prevRRef = dissectPlanCount > 0 ? dissectPlan[0].rRef : orb_real_t(1);

        for (int level = 1; level <= dissectPlanCount; level++)
        {
            DissectionEntry active = dissectPlan[level - 1];
            PointGroup levelGroups[kMaxConfigSubshells];
            int levelGroupCount = buildDissectGroups(level, levelGroups);
            int visibleCount = 0;
            for (int g = 0; g < levelGroupCount; g++)
                visibleCount += levelGroups[g].count;
            AtomScale s = scaleForAtom(active.rRef);

            char bigLabel[8];
            std::snprintf(bigLabel, sizeof(bigLabel), "%d%c", active.n, subshellLabelChar(active.ell));
            char caption[40];
            std::snprintf(caption, sizeof(caption), "%s (%d/%d)", elementSymbol(preset.z), level, dissectPlanCount);
            auto title = [&](uint16_t *fb, int x, int y, uint16_t color)
            {
                drawDissectTitle(fb, x, y, color, bigLabel, caption, active.occ);
            };

            uint32_t flyMs = dissectFlyDurationMs(prevRRef, active.rRef);
            ESP_LOGI(kAtomViewTag, "dissecting shell %d%c (%d pts, level %d/%d, fly %ums)", active.n,
                     subshellLabelChar(active.ell), visibleCount, level, dissectPlanCount, flyMs);

            bool completed = easeScaleTimed(display, preset.points, levelGroups, levelGroupCount, title, protonColor,
                                            textColor, scaleBarColor, camera, scale, s.baseScale, flyMs, &tilt,
                                            kHiddenPointsThreshold);
            scale = s.baseScale;
            prevRRef = active.rRef;
            if (!completed)
            {
                ESP_LOGI(kAtomViewTag, "dissection aborted -- movement detected mid-fly");
                break;
            }

            int64_t holdStartUs = esp_timer_get_time();
            uint32_t holdFrameSalt = 0;
            bool aborted = false;
            while (esp_timer_get_time() - holdStartUs < kDissectHoldUs)
            {
                if (tilt.poll().phase != TiltPhase::kIdle)
                {
                    ESP_LOGI(kAtomViewTag, "dissection aborted -- movement detected during hold");
                    aborted = true;
                    break;
                }
                renderDissectFrame(display, preset.points, levelGroups, levelGroupCount, protonColor, textColor,
                                   scaleBarColor, camera, scale, bigLabel, caption, active.occ, holdFrameSalt,
                                   kHiddenPointsThreshold);
                stepCamera(&camera);
                holdFrameSalt++;
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (aborted)
                break;
        }

        // Ease back out to the full view, at the same fixed pm/s pace (a single hop covering
        // the full innermost-to-outermost distance, since we're returning straight to the full
        // view) -- preset.points/preset.groups are already the full, untouched cloud.
        uint32_t returnFlyMs = dissectFlyDurationMs(prevRRef, dissectPlanCount > 0 ? dissectPlan[0].rRef : prevRRef);
        auto fullTitle = [&](uint16_t *fb, int x, int y, uint16_t color)
        {
            drawAtomTitle(fb, x, y, preset.z, color);
        };
        easeScaleTimed(display, preset.points, preset.groups, preset.groupCount, fullTitle, protonColor, textColor,
                       scaleBarColor, camera, scale, preset.baseScale, returnFlyMs, nullptr, kHiddenPointsThreshold);
    }

    /// Like camera.h's flyOver(), but redraws the proton marker on top of every frame after the
    /// cloud (see drawAtomProtonMarker()'s docstring). Used for the boot intro, element
    /// switches, and random zoom excursions; easeScaleTimed()/renderDissectFrame() above get
    /// the same redraw inline since they're already local to this file.
    template <typename TitleDrawFn>
    void atomFlyOver(Display &display, const AtomPoint *points, const PointGroup *groups, int groupCount,
                     TitleDrawFn drawTitle, uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor,
                     CameraState *camera, orb_real_t startScale, orb_real_t endScale, int frames,
                     uint32_t buzzThreshold = 0)
    {
        for (int i = 0; i < frames; i++)
        {
            orb_real_t t = frames > 1 ? orb_real_t(i) / orb_real_t(frames - 1) : orb_real_t(1);
            orb_real_t scale = startScale + (endScale - startScale) * t;

            display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
            renderSceneGrouped(display.getFrameBuf(), points, groups, groupCount, protonColor, *camera, scale,
                               uint32_t(i), buzzThreshold);
            drawAtomProtonMarker(display.getFrameBuf(), protonColor);
            drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
            drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
            display.presentFrame();

            stepCamera(camera);
        }
    }
} // namespace

void runAtomView(Display &display, TiltGestureDetector &tilt)
{
    ESP_LOGI(kAtomViewTag, "display ready, Z=1..%d available", kMaxDisplayZ);

    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM: this struct (~94KB of points, plus a
    // negligible few hundred bytes of per-subshell ranges/groups -- see AtomPresetState's
    // docstring) is large enough that placing it in internal RAM left too little contiguous
    // DMA-capable memory for Display::Display()'s frame-buffer allocation, which aborted at
    // boot.
    static EXT_RAM_BSS_ATTR AtomPresetState preset;
    if (preset.z == 0)                  // first-ever call this boot -- later calls (after a menu round-trip)
        preset.load(kAtomViewDefaultZ); // keep whatever element was last showing
    refreshDissectPlan(preset);

    // preset has static storage duration, so it's odr-usable without capturing -- see
    // orbital_view.cpp's drawTitle for the same pattern.
    auto drawTitle = [](uint16_t *frameBuf, int x, int y, uint16_t color)
    {
        drawAtomTitle(frameBuf, x, y, preset.z, color);
        // Add the Z number in the top-right corner, so the user can see it while browsing the periodic table.
        char zLabel[4];
        std::snprintf(zLabel, sizeof(zLabel), "%d", preset.z);
        int zX = Display::kDisplayWidth - textWidth(zLabel, kFontHuge) - 10;
        drawText(frameBuf, zX, 10, zLabel, Display::kColorOrbitalRed, kFontHuge);
    };

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    atomFlyOver(display, preset.points, preset.groups, preset.groupCount, drawTitle, kProtonColor, kTextColor,
                kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames,
                kHiddenPointsThreshold);

    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();
    uint32_t buzzFrame = 0; // per-frame salt for renderSceneGrouped()'s hidden-points buzz, see camera.h
    int zoomExcursionCountdown = nextZoomExcursionCountdown();
    int64_t lastActivityUs = esp_timer_get_time();
    // Caps idle auto-advance to at most one dissection per element before it's forced to jump,
    // so idle browsing doesn't get stuck re-dissecting the same element every kViewIdleJumpUs.
    // Reset to false whenever a new element loads, see switchToElement() below.
    bool idleDissectedThisElement = false;

    // Shared by every switch site below (Up/Down/Left/Right periodic-table movement, and the
    // idle random jump).
    auto switchToElement = [&](int newZ)
    {
        ESP_LOGI(kAtomViewTag, "switching element Z %d -> %d (%s)", preset.z, newZ, elementNameIt(newZ));
        orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        scrollElementIntro(display, elementNameIt(newZ), newZ, elementSymbol(newZ), kAccentColor);
        preset.load(newZ);
        refreshDissectPlan(preset);
        idleDissectedThisElement = false; // fresh element -- fresh idle dissection budget
        atomFlyOver(display, preset.points, preset.groups, preset.groupCount, drawTitle, kProtonColor, kTextColor,
                    kScaleBarColor, &camera, currentScale, preset.baseScale, kSwitchTransitionFrames,
                    kHiddenPointsThreshold);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
        // scrollElementIntro()'s real-time holds plus the flyOver() above spend real wall-clock
        // time without incrementing frameCount; reset the FPS window here so it only ever
        // measures steady-state frames instead of charging that idle time to a later window.
        frameCount = 0;
        fpsWindowStartUs = esp_timer_get_time();
    };

    while (true)
    {
        screenshot_pause::checkpoint(); // see screenshot_pause.h -- lets a screenshot capture happen safely

        TiltEvent tiltEv = tilt.poll();
        if (tiltEv.phase == TiltPhase::kConfirmed)
            lastActivityUs = esp_timer_get_time();

        // A single axis (Up/Down) walks the whole periodic table in snake order (see
        // periodicTableSnakeStep()), freeing Left/Right for menu-return/dissection.
        if (tiltEv.phase == TiltPhase::kConfirmed)
        {
            if (tiltEv.direction == TiltDirection::kUp || tiltEv.direction == TiltDirection::kDown)
            {
                int delta = tiltEv.direction == TiltDirection::kDown ? 1 : -1;
                int newZ = periodicTableSnakeStep(preset.z, delta);
                ESP_LOGI(kAtomViewTag, "tilt %s confirmed -- periodic-table move", tiltDirectionName(tiltEv.direction));
                switchToElement(newZ);
                continue;
            }
            if (tiltEv.direction == TiltDirection::kLeft)
            {
                ESP_LOGI(kAtomViewTag, "tilt LEFT confirmed -- returning to menu");
                return;
            }
            if (tiltEv.direction == TiltDirection::kRight)
            {
                if (dissectPlanCount > 0)
                {
                    ESP_LOGI(kAtomViewTag, "tilt RIGHT confirmed -- starting automatic dissection (%d shells)",
                             dissectPlanCount);
                    runDissectionSequence(display, preset, camera, kProtonColor, kTextColor, kScaleBarColor, tilt);
                }
                else
                {
                    ESP_LOGI(kAtomViewTag, "tilt RIGHT confirmed -- no subshells to dissect");
                }
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                frameCount = 0; // see switchToElement()'s FPS-window comment above
                fpsWindowStartUs = esp_timer_get_time();
                continue;
            }
        }

        // Idle auto-advance: each idle timeout has a coin-flip chance to dissect the current
        // element instead of jumping, but only once per element (idleDissectedThisElement) and
        // only when there's something to dissect; once that budget is used (or there's nothing
        // to dissect), idle timeouts always jump.
        if (esp_timer_get_time() - lastActivityUs > kViewIdleJumpUs)
        {
            bool canDissect = !idleDissectedThisElement && dissectPlanCount > 0;
            if (canDissect && randomUnit() < orb_real_t(0.5))
            {
                ESP_LOGI(kAtomViewTag, "idle 60s+ -- dissecting current element (Z=%d, %d shells)", preset.z,
                         dissectPlanCount);
                runDissectionSequence(display, preset, camera, kProtonColor, kTextColor, kScaleBarColor, tilt);
                idleDissectedThisElement = true;
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                frameCount = 0; // see switchToElement()'s FPS-window comment above
                fpsWindowStartUs = esp_timer_get_time();
            }
            else
            {
                int newZ = randomIndexExcluding(preset.z - 1, kMaxDisplayZ) + 1;
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
        if (zoomExcursionCountdown <= 0)
        {
            orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
            orb_real_t targetScale =
                preset.baseScale * randomUniform(kZoomExcursionScaleMinFactor, kZoomExcursionScaleMaxFactor);
            atomFlyOver(display, preset.points, preset.groups, preset.groupCount, drawTitle, kProtonColor, kTextColor,
                        kScaleBarColor, &camera, currentScale, targetScale, kZoomExcursionEaseFrames,
                        kHiddenPointsThreshold);
            atomFlyOver(display, preset.points, preset.groups, preset.groupCount, drawTitle, kProtonColor, kTextColor,
                        kScaleBarColor, &camera, targetScale, preset.baseScale, kZoomExcursionEaseFrames,
                        kHiddenPointsThreshold);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
            frameCount = 0; // see switchToElement()'s FPS-window comment above
            fpsWindowStartUs = esp_timer_get_time();
            continue;
        }

        orb_real_t scale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        renderSceneGrouped(display.getFrameBuf(), preset.points, preset.groups, preset.groupCount, kProtonColor,
                           camera, scale, buzzFrame, kHiddenPointsThreshold);
        drawAtomProtonMarker(display.getFrameBuf(), kProtonColor); // see its docstring -- keep it visible over the cloud
        buzzFrame = buzzFrame < 1000000u ? buzzFrame + 1 : 0;
        drawAtomTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, preset.z, kTextColor);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
        if (tiltEv.phase != TiltPhase::kIdle)
            drawTiltArrow(display.getFrameBuf(), tiltEv.direction, kAccentColor);
        display.presentFrame();

        frameCount++;
        if (frameCount >= kFpsUpdateInterval)
        {
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
