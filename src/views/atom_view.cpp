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
#include "debug/frame_stats.h"
#include "debug/screenshot_pause.h"
#include "physics/slater.h"
#include "config/visual_constants.h" // kAccentColor, kViewIdleJumpUs, kAtomProtonMarkerSize, kBoundingCircleColor, kElementIntro*, kDissect*, kFpsUpdateInterval

static const char *kAtomViewTag = "atom_view";

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
    rRef = scale.rRef;
    z = zIn;

    loadMs = (esp_timer_get_time() - startUs) / 1000;
    ESP_LOGI(kAtomViewTag, "%s loaded in %lldms, outer=%d%c, outerRBohr=%.2f, scale=%.1f, outerRPx=%.1f",
             elementSymbol(zIn), loadMs, outer.n, subshellLabelChar(outer.ell), double(outer.rRef),
             double(baseScale), double(outer.rRef * baseScale));
}

void drawAtomTitle(Display &display, int x, int y, int z, uint16_t textColor)
{
    drawText(display, x, y, elementSymbol(z), textColor, kFontHuge);
}

void renderAtomFrame(Display &display, const AtomPresetState &preset, const CameraState &camera, orb_real_t scale,
                     uint32_t frameSalt, uint32_t buzzThreshold)
{
    // Nucleus drawn BEFORE the cloud by renderSceneGrouped() (matching pc/viewer_common.py's
    // blend order on purpose, see camera.h), so a point landing on the same pixel can
    // alpha-blend over it and dim/hide it near the origin -- the drawProtonMarker() call below
    // redraws it opaque on top, after the cloud, so it's always visible.
    renderSceneGrouped(display, preset.points, preset.groups, preset.groupCount, kProtonColor, camera, scale,
                       frameSalt, buzzThreshold);
    drawProtonMarker(display, kProtonColor, kAtomProtonMarkerSize);
    drawBoundingCircle(display, preset.rRef, scale, kBoundingCircleColor);
    drawAtomTitle(display, kTitleTextX, kTitleTextY, preset.z, kTextColor);
    // Z number in the top-right corner, so the user can see it while browsing the periodic table.
    char zLabel[4];
    std::snprintf(zLabel, sizeof(zLabel), "%d", preset.z);
    int zX = Display::kDisplayWidth - textWidth(zLabel, kFontHuge);
    drawText(display, zX, kTitleTextY, zLabel, Display::kColorGreenYellow, kFontHuge);
    drawScaleBar(display, scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
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
            display.clearScreen();
            drawTextScaled(display, symbolX, symbolY, symbol, kElementIntroSymbolColor, kFontHuge,
                           kElementIntroSymbolScale);
            if (showName)
            {
                drawTextScaled(display, x, nameY, nameIt, nameColor, kFontLarge, nameScale);
                drawText(display, zX, zY, zLabel, nameColor, kFontHuge);
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

// --- On-device shell dissection (Right tilt-hold) -- see atom_view.h's header comment ---
//
// Runs as one self-contained blocking sequence once triggered, not one dissect step per
// hold: a single Right-hold peels through every occupied subshell automatically (eased zoom +
// a real-time hold on each, continuously tumbling throughout) and returns to the full atom at
// the end, with no further gesture needed mid-sequence. A second Right tilt-hold (confirmed,
// same ~1s threshold as any other gesture) cancels early and eases straight back to the full
// atom instead -- any other direction is ignored while a dissection is in progress.

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
     * Takes `plan`/`planCount` explicitly (rather than reading the file-local
     * dissectPlan/dissectPlanCount below directly) so it has no hidden dependency on which
     * AtomPresetState last called refreshDissectPlan() -- screenshot_batch.cpp's dissection
     * gallery builds its own local plan for its own preset and calls this the same way
     * runDissectionSequence() below does.
     *
     * @param outGroups  [out] Must hold at least kMaxConfigSubshells entries.
     * @return           Number of groups written (== planCount - (level - 1)).
     */
    int buildDissectGroups(const DissectionEntry *plan, int planCount, int level, PointGroup *outGroups)
    {
        int written = 0;
        for (int rank = level - 1; rank < planCount; rank++)
        {
            const DissectionEntry &e = plan[rank];
            uint16_t color;
            if (rank == level - 1)
            {
                // Brightened well past the shell's plain color (kDissectActiveBrighten, more
                // than colorizeAtomSubshells()'s own kAtomOuterShellBrighten) so the actively
                // dissected shell reads as clearly "lit up" against the flat-gray peeled-away
                // shells around it.
                const uint8_t *base = shellBaseRgb(e.n);
                color = Display::packColor565(brightenChannel(base[0], kDissectActiveBrighten),
                                              brightenChannel(base[1], kDissectActiveBrighten),
                                              brightenChannel(base[2], kDissectActiveBrighten));
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
    void drawDissectTitle(Display &display, int x, int y, uint16_t color, const char *bigLabel, const char *caption,
                          int occ)
    {
        drawText(display, x, y, bigLabel, color, kFontHuge);
        drawText(display, x, y + kFontHuge.height, caption, color, kFontLarge);

        char occText[8];
        std::snprintf(occText, sizeof(occText), "%d\x7F", occ);
        int occX = Display::kDisplayWidth - textWidth(occText, kFontHuge) - kDissectOccMarginPx;
        drawText(display, occX, kDissectOccMarginPx, occText, Display::kColorOrbitalBlue, kFontHuge);
    }

    /// Render one frame at a fixed `scale`, for the real-time hold (which doesn't need
    /// easing). `frameSalt`/`buzzThreshold` feed renderScene()'s hidden-points buzz (see
    /// camera.h's kHiddenPointsFraction); the caller owns the per-call frameSalt counter.
    void renderDissectFrame(Display &display, const AtomPoint *points, const PointGroup *groups, int groupCount,
                            uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, const CameraState &camera,
                            orb_real_t scale, orb_real_t rRef, const char *bigLabel, const char *caption, int occ,
                            uint32_t frameSalt = 0, uint32_t buzzThreshold = 0)
    {
        display.waitForFlushDone();
        renderSceneGrouped(display, points, groups, groupCount, protonColor, camera, scale, frameSalt,
                           buzzThreshold);
        drawProtonMarker(display, protonColor, kAtomProtonMarkerSize);
        drawBoundingCircle(display, rRef, scale, kBoundingCircleColor);
        drawDissectTitle(display, kTitleTextX, kTitleTextY, textColor, bigLabel, caption, occ);
        drawScaleBar(display, scale / kPmPerBohr, "pm", scaleBarColor, textColor);
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
     * If `tilt` is non-null, it's polled every frame; a confirmed Right tilt-hold (the same
     * gesture that starts a dissection, ~1s) aborts immediately and returns false, cancelling
     * the dissection in progress and sending it back to the full atom -- any other direction is
     * ignored so an incidental nudge mid-sequence doesn't cut it short. The default nullptr is
     * used for the sequence's own final "back to full view" leg, which is the closing action
     * and should not itself be interruptible.
     */
    template <typename TitleDrawFn>
    bool easeScaleTimed(Display &display, const AtomPoint *points, const PointGroup *groups, int groupCount,
                        TitleDrawFn drawTitle, uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor,
                        CameraState &camera, orb_real_t startScale, orb_real_t endScale, orb_real_t rRef,
                        uint32_t durationMs, TiltGestureDetector *tilt = nullptr, uint32_t buzzThreshold = 0)
    {
        int64_t startUs = esp_timer_get_time();
        int64_t durationUs = int64_t(durationMs) * 1000;
        uint32_t frameSalt = 0; // real-time loop, not a fixed frame count -- own counter, see camera.h's flyOver()
        for (;;)
        {
            if (tilt)
            {
                TiltEvent ev = tilt->poll();
                if (ev.phase == TiltPhase::kConfirmed && ev.direction == TiltDirection::kRight)
                    return false;
            }

            int64_t elapsedUs = esp_timer_get_time() - startUs;
            orb_real_t t = durationUs > 0 ? orb_real_t(double(elapsedUs) / double(durationUs)) : orb_real_t(1);
            if (t > orb_real_t(1))
                t = orb_real_t(1);
            orb_real_t scale = startScale + (endScale - startScale) * t;

            display.waitForFlushDone();
            renderSceneGrouped(display, points, groups, groupCount, protonColor, camera, scale, frameSalt,
                               buzzThreshold);
            drawProtonMarker(display, protonColor, kAtomProtonMarkerSize); // keep it visible over the cloud
            drawBoundingCircle(display, rRef, scale, kBoundingCircleColor);
            drawTitle(display, kTitleTextX, kTitleTextY, textColor);
            drawScaleBar(display, scale / kPmPerBohr, "pm", scaleBarColor, textColor);
            display.presentFrame();

            stepCamera(&camera);
            frameSalt++;
            if (t >= orb_real_t(1))
                break;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return true;
    }

    /// Tile the electron symbol across the whole frame in a dim, unobtrusive grid -- drawn first
    /// so the title lines composite on top of it (plain overwrite, no blending, matching every
    /// other draw function in this project).
    void drawElectronBackdrop(Display &display)
    {
        for (int y = 6; y < Display::kDisplayHeight; y += kDissectIntroBgSpacingY)
            for (int x = 6; x < Display::kDisplayWidth; x += kDissectIntroBgSpacingX)
                drawText(display, x, y, kGlyphElectron, kDissectIntroBgColor, kFontSmall);
    }

    /// Largest scale (up to kDissectIntroMaxWordScale) that keeps all three of `line1`/`line2`/
    /// `name` inside the panel width -- same fallback-by-shrinking approach as pickNameScale()
    /// above, just checked against all three strings at once so the reveal never jumps to a
    /// different size between stages.
    int pickDissectIntroScale(const char *line1, const char *line2, const char *name)
    {
        int maxWidth = Display::kDisplayWidth - 2 * kDissectIntroMarginPx;
        for (int scale = kDissectIntroMaxWordScale; scale > 1; scale--)
            if (textWidthScaled(line1, kFontLarge, scale) <= maxWidth &&
                textWidthScaled(line2, kFontLarge, scale) <= maxWidth &&
                textWidthScaled(name, kFontLarge, scale) <= maxWidth)
                return scale;
        return 1;
    }

    /// 3-line "Configurazione / elettronica / <nameIt>" title card over the electron backdrop,
    /// revealed one line at a time (same staged-reveal technique as orbital_view.cpp's
    /// scrollOrbitalIntro()) so it reads as building up rather than flashing on all at once --
    /// the 3 stages together span kDissectIntroHoldMs before the dissection sequence starts.
    void showElectronConfigIntro(Display &display, const char *nameIt)
    {
        constexpr const char *kLine1 = "Configurazione";
        constexpr const char *kLine2 = "elettronica";

        int scale = pickDissectIntroScale(kLine1, kLine2, nameIt);
        int lineH = kFontLarge.height * scale;
        int y1 = kDissectIntroFirstLineY;
        int y2 = y1 + lineH + kDissectIntroLineGapPx;
        int y3 = y2 + lineH + kDissectIntroLineGapPx;
        int x1 = (Display::kDisplayWidth - textWidthScaled(kLine1, kFontLarge, scale)) / 2;
        int x2 = (Display::kDisplayWidth - textWidthScaled(kLine2, kFontLarge, scale)) / 2;
        int x3 = (Display::kDisplayWidth - textWidthScaled(nameIt, kFontLarge, scale)) / 2;

        auto renderStage = [&](int linesShown)
        {
            display.waitForFlushDone();
            display.clearScreen();
            drawElectronBackdrop(display);
            if (linesShown >= 1)
                drawTextScaled(display, x1, y1, kLine1, kAccentColor, kFontLarge, scale);
            if (linesShown >= 2)
                drawTextScaled(display, x2, y2, kLine2, kAccentColor, kFontLarge, scale);
            if (linesShown >= 3)
                drawTextScaled(display, x3, y3, nameIt, kAccentColor, kFontLarge, scale);
            display.presentFrame();
        };

        renderStage(1);
        vTaskDelay(pdMS_TO_TICKS(kDissectIntroStageHoldMs));
        renderStage(2);
        vTaskDelay(pdMS_TO_TICKS(kDissectIntroStageHoldMs));
        renderStage(3);
        vTaskDelay(pdMS_TO_TICKS(kDissectIntroHoldMs - 2 * kDissectIntroStageHoldMs));
    }

    /**
     * @brief Automatically peel through every occupied subshell, outer to inner.
     *
     * For each level: ease the camera in (easeScaleTimed(), paced by real time) to frame that
     * subshell with its label, hold for kDissectHoldUs while continuing to tumble, then move to
     * the next level. `tilt` is polled every eased and held frame; a confirmed Right tilt-hold
     * breaks out of the level loop early (the same gesture that started the dissection cancels
     * it). Either way (completed or cancelled) execution falls through to the same tail: ease
     * back out to preset.baseScale using preset's own unmodified render groups, then hold there
     * for kDissectPostReturnHoldUs so the sequence never runs straight into another transition.
     * Unlike an earlier revision of this function, `preset.points`/`preset.groups` are never
     * touched during the loop -- buildDissectGroups() only ever reads dissectPlan's point
     * ranges into a level-local group list, so there's nothing to rebuild/undo here, and a
     * cancelled dissection lands back on the full element exactly like a finished one does.
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
            int levelGroupCount = buildDissectGroups(dissectPlan, dissectPlanCount, level, levelGroups);
            int visibleCount = 0;
            for (int g = 0; g < levelGroupCount; g++)
                visibleCount += levelGroups[g].count;
            AtomScale s = scaleForAtom(active.rRef);

            char bigLabel[8];
            std::snprintf(bigLabel, sizeof(bigLabel), "%d%c", active.n, subshellLabelChar(active.ell));
            char caption[40];
            std::snprintf(caption, sizeof(caption), "%s (%d/%d)", elementSymbol(preset.z), level, dissectPlanCount);
            auto title = [&](Display &d, int x, int y, uint16_t color)
            {
                drawDissectTitle(d, x, y, color, bigLabel, caption, active.occ);
            };

            uint32_t flyMs = dissectFlyDurationMs(prevRRef, active.rRef);
            ESP_LOGI(kAtomViewTag, "dissecting shell %d%c (%d pts, level %d/%d, fly %ums)", active.n,
                     subshellLabelChar(active.ell), visibleCount, level, dissectPlanCount, flyMs);

            bool completed = easeScaleTimed(display, preset.points, levelGroups, levelGroupCount, title, protonColor,
                                            textColor, scaleBarColor, camera, scale, s.baseScale, active.rRef, flyMs,
                                            &tilt, kHiddenPointsThreshold);
            scale = s.baseScale;
            prevRRef = active.rRef;
            if (!completed)
            {
                ESP_LOGI(kAtomViewTag, "dissection cancelled -- tilt RIGHT confirmed mid-fly");
                break;
            }

            int64_t holdStartUs = esp_timer_get_time();
            uint32_t holdFrameSalt = 0;
            bool aborted = false;
            while (esp_timer_get_time() - holdStartUs < kDissectHoldUs)
            {
                TiltEvent ev = tilt.poll();
                if (ev.phase == TiltPhase::kConfirmed && ev.direction == TiltDirection::kRight)
                {
                    ESP_LOGI(kAtomViewTag, "dissection cancelled -- tilt RIGHT confirmed during hold");
                    aborted = true;
                    break;
                }
                renderDissectFrame(display, preset.points, levelGroups, levelGroupCount, protonColor, textColor,
                                   scaleBarColor, camera, scale, active.rRef, bigLabel, caption, active.occ,
                                   holdFrameSalt, kHiddenPointsThreshold);
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
        auto fullTitle = [&](Display &d, int x, int y, uint16_t color)
        {
            drawAtomTitle(d, x, y, preset.z, color);
        };
        easeScaleTimed(display, preset.points, preset.groups, preset.groupCount, fullTitle, protonColor, textColor,
                       scaleBarColor, camera, scale, preset.baseScale, preset.rRef, returnFlyMs, nullptr,
                       kHiddenPointsThreshold);

        // Brief settle hold on the full atom (still tumbling) before returning control, so a
        // completed or cancelled dissection never runs straight into another element switch or
        // idle jump -- at least kDissectPostReturnHoldUs of real time between the zoom-out
        // finishing and whatever runAtomView() does next. Not itself interruptible, same as the
        // return leg above -- it's the sequence's own closing beat, not a gesture-reactive one.
        int64_t postHoldStartUs = esp_timer_get_time();
        uint32_t postHoldFrameSalt = 0;
        while (esp_timer_get_time() - postHoldStartUs < kDissectPostReturnHoldUs)
        {
            display.waitForFlushDone();
            renderAtomFrame(display, preset, camera, preset.baseScale, postHoldFrameSalt, kHiddenPointsThreshold);
            display.presentFrame();
            stepCamera(&camera);
            postHoldFrameSalt++;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    /// Like camera.h's flyOver(), but calls renderAtomFrame() (proton marker redrawn opaque
    /// on top, bounding circle, title + Z-number label, scale bar) instead of camera.h's own
    /// generic per-frame draw. Used for the boot intro, element switches, and random zoom
    /// excursions; easeScaleTimed()/renderDissectFrame() above get the same redraw inline
    /// since they need the dissection-specific title instead.
    void atomFlyOver(Display &display, const AtomPresetState &preset, CameraState *camera, orb_real_t startScale,
                     orb_real_t endScale, int frames, uint32_t buzzThreshold = 0)
    {
        for (int i = 0; i < frames; i++)
        {
            orb_real_t t = frames > 1 ? orb_real_t(i) / orb_real_t(frames - 1) : orb_real_t(1);
            orb_real_t scale = startScale + (endScale - startScale) * t;

            display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
            renderAtomFrame(display, preset, *camera, scale, uint32_t(i), buzzThreshold);
            display.presentFrame();

            stepCamera(camera);
        }
    }
} // namespace

int renderAtomDissectFrame(Display &display, const AtomPresetState &preset, const CameraState &camera, int level)
{
    DissectionEntry plan[kMaxConfigSubshells];
    int planCount = subshellDissectionPlan(preset.points, preset.ranges, preset.rangeCount, plan);
    if (level < 1 || level > planCount)
        return planCount;

    const DissectionEntry &active = plan[level - 1];
    PointGroup groups[kMaxConfigSubshells];
    int groupCount = buildDissectGroups(plan, planCount, level, groups);
    // Re-normalized to this shell's own radius, same as buildDissectGroups()'s live callers
    // (runDissectionSequence()'s hold phase) and AtomPresetState::load()'s full-atom scale.
    AtomScale s = scaleForAtom(active.rRef);

    char bigLabel[8];
    std::snprintf(bigLabel, sizeof(bigLabel), "%d%c", active.n, subshellLabelChar(active.ell));
    char caption[40];
    std::snprintf(caption, sizeof(caption), "%s (%d/%d)", elementSymbol(preset.z), level, planCount);

    renderSceneGrouped(display, preset.points, groups, groupCount, kProtonColor, camera, s.baseScale);
    drawProtonMarker(display, kProtonColor, kAtomProtonMarkerSize);
    drawBoundingCircle(display, active.rRef, s.baseScale, kBoundingCircleColor);
    drawDissectTitle(display, kTitleTextX, kTitleTextY, kTextColor, bigLabel, caption, active.occ);
    drawScaleBar(display, s.baseScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
    return planCount;
}

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

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    atomFlyOver(display, preset, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames,
                kHiddenPointsThreshold);

    FrameStats stats; // FPS + render/prepare moving averages + last-load-ms + free IRAM, see debug/frame_stats.h
    stats.reset();
    stats.lastLoadMs = preset.loadMs;
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
        stats.lastLoadMs = preset.loadMs;
        refreshDissectPlan(preset);
        idleDissectedThisElement = false; // fresh element -- fresh idle dissection budget
        atomFlyOver(display, preset, &camera, currentScale, preset.baseScale, kSwitchTransitionFrames,
                    kHiddenPointsThreshold);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
        // scrollElementIntro()'s real-time holds plus the flyOver() above spend real wall-clock
        // time without incrementing frameCount; reset the FPS window here so it only ever
        // measures steady-state frames instead of charging that idle time to a later window.
        stats.reset();
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
                stats.reset(); // see switchToElement()'s FPS-window comment above
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
                stats.reset(); // see switchToElement()'s FPS-window comment above
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
            atomFlyOver(display, preset, &camera, currentScale, targetScale, kZoomExcursionEaseFrames,
                        kHiddenPointsThreshold);
            atomFlyOver(display, preset, &camera, targetScale, preset.baseScale, kZoomExcursionEaseFrames,
                        kHiddenPointsThreshold);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
            stats.reset(); // see switchToElement()'s FPS-window comment above
            continue;
        }

        orb_real_t scale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        int64_t tBeforeWait = esp_timer_get_time();
        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        int64_t tAfterWait = esp_timer_get_time();
        renderAtomFrame(display, preset, camera, scale, buzzFrame, kHiddenPointsThreshold);
        buzzFrame = buzzFrame < 1000000u ? buzzFrame + 1 : 0;
        if (tiltEv.phase != TiltPhase::kIdle)
            drawTiltArrow(display, tiltEv.direction, kAccentColor);
        display.presentFrame();
        int64_t tAfterPresent = esp_timer_get_time();

        stats.recordFrame(double(tAfterWait - tBeforeWait) / 1000.0, double(tAfterPresent - tAfterWait) / 1000.0);
        stats.maybeLog(kAtomViewTag);

        stepCamera(&camera);
        zoomAngle += kZoomAngleStep;
        if (zoomAngle >= kTwoPi)
            zoomAngle -= kTwoPi;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
