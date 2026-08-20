#include "views/orbital_view.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>

#include "render/equation_bitmap.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_log.h"
#include "esp_timer.h"
#include "render/font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "physics/orbital_library.h"
#include "render/overlay.h"
#include "views/orbital_slice.h"
#include "debug/frame_stats.h"
#include "debug/screenshot_pause.h"
#include "config/visual_constants.h" // kViewIdleJumpUs, kOrbitalIntro*, kOrbitalProtonMarkerSize, etc.

static const char *kOrbitalViewTag = "orbital_view";

namespace
{

    void renderOrbitalIntroStage(Display &display, const char *text)
    {
        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        display.clearScreen();
        drawEquationBackdrop(frameBuf, kOrbitalIntroEqX, kOrbitalIntroEqY, kOrbitalIntroEqColor);
        int width = textWidthScaled(text, kFontLarge, kOrbitalIntroNumberScale);
        int x = (Display::kDisplayWidth - width) / 2;
        drawTextScaled(frameBuf, x, kOrbitalIntroNumberY, text, Display::kColorWhite, kFontLarge,
                       kOrbitalIntroNumberScale);
        display.presentFrame();
    }

    /// Reveal "n=X", then "n=X l=Y", then "n=X l=Y m=Z" (each held kOrbitalIntroStageHoldMs) over
    /// the dim equation backdrop -- same spot in the flow as atom_view.cpp's scrollElementIntro()
    /// before its flyOver(). Holds an extra 500ms after the final stage so the reveal doesn't
    /// rush straight into the switch.
    void scrollOrbitalIntro(Display &display, int n, int ell, int m)
    {
        char stage[24];
        std::snprintf(stage, sizeof(stage), "n=%d", n);
        renderOrbitalIntroStage(display, stage);
        vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs));

        std::snprintf(stage, sizeof(stage), "n=%d %s=%d", n, kGlyphScriptL, ell);
        renderOrbitalIntroStage(display, stage);
        vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs));

        std::snprintf(stage, sizeof(stage), "n=%d %s=%d m=%d", n, kGlyphScriptL, ell, m);
        renderOrbitalIntroStage(display, stage);
        vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs + 500));
    }

} // namespace

// --- On-device plane-slice heatmap (Right tilt-hold) -- see orbital_view.h's header comment,
// SLICE.md for the full design -- mirrors atom_view.cpp's runDissectionSequence() split: this
// file orchestrates the gesture sequence, orbital_slice.h/.cpp owns the physics table build
// and per-frame draw.

namespace
{
    /// Static "Sezione" title card (D4, SLICE.md) over the same dim equation backdrop
    /// scrollOrbitalIntro() uses, held kSliceIntroHoldMs before the slice build starts.
    void showSliceIntro(Display &display, const OrbitalPresetState &preset)
    {
        constexpr const char *kSliceLabel = "Sezione";

        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        display.clearScreen();
        drawEquationBackdrop(frameBuf, kOrbitalIntroEqX, kOrbitalIntroEqY, kOrbitalIntroEqColor);
        // kOrbitalIntroNumberY is tuned for scrollOrbitalIntro()'s single scaled kFontLarge
        // line -- this card stacks a full kFontHuge line above a kFontLarge one, so it needs
        // its own, taller layout to stay clear of the panel's bottom edge (240px).
        int labelY = kOrbitalIntroEqY + kEquationBitmapHeight + 10;
        int numbersY = labelY + kFontHuge.height + 4;
        int labelWidth = textWidth(kSliceLabel, kFontHuge);
        drawText(frameBuf, (Display::kDisplayWidth - labelWidth) / 2, labelY, kSliceLabel, kAccentColor, kFontHuge);
        int numbersWidth = textWidth(preset.orbital_numbers, kFontLarge);
        drawText(frameBuf, (Display::kDisplayWidth - numbersWidth) / 2, numbersY, preset.orbital_numbers, kTextColor,
                 kFontLarge);
        display.presentFrame();

        vTaskDelay(pdMS_TO_TICKS(kSliceIntroHoldMs));
    }

    /// Preset title top-left, quantum numbers bottom-right, scale bar bottom-left -- same
    /// layout renderOrbitalFrame() uses for the 3D cloud, so the slice reads as the same
    /// object's own view, not a separate mode with its own conventions. A small "densita di
    /// probabilita" legend sits right under the title: since D1's revision the heatmap is a
    /// pure density plot (no phase/sign color, unlike the 3D cloud), so it's worth naming what
    /// the color ramp actually encodes.
    void drawSliceOverlay(uint16_t *frameBuf, const OrbitalPresetState &preset, orb_real_t extentPm)
    {
        drawText(frameBuf, kTitleTextX, kTitleTextY, preset.title, kTextColor, kFontHuge);
        constexpr const char *kSliceLegend = "densita di probabilita";
        drawText(frameBuf, kTitleTextX, kTitleTextY + kFontHuge.height + 2, kSliceLegend, kScaleBarColor, kFontSmall);
        int width = textWidth(preset.orbital_numbers, kFontLarge);
        int height = kFontLarge.height;
        drawText(frameBuf, Display::kDisplayWidth - width, Display::kDisplayHeight - height - 15,
                 preset.orbital_numbers, kTextColor, kFontLarge);
        // Half the panel width spans extentPm (SliceTable's own framing, see buildSliceTable()).
        orb_real_t pixelsPerPm = orb_real_t(Display::kDisplayWidth) / (orb_real_t(2) * extentPm);
        drawScaleBar(frameBuf, pixelsPerPm, "pm", kScaleBarColor, kTextColor);
    }

    /**
     * @brief Automatic slice sequence: intro card -> build -> fade in -> hold -> fade out.
     *
     * One gesture starts the whole thing; `tilt` is polled every fade-in/hold frame and any
     * non-idle phase (the device starting to tip, not necessarily a full confirmed hold) cuts
     * the sequence short -- but the closing fade-out leg below always still runs afterward
     * (from wherever the fade level was cut off), so the sequence always lands back on the
     * untouched 3D view smoothly, exactly like runDissectionSequence()'s unconditional
     * ease-back tail. No camera motion at any point (D2, SLICE.md): the plane is static and
     * the 3D camera is left alone so it resumes seamlessly once this returns.
     */
    void runSliceSequence(Display &display, OrbitalPresetState &preset, TiltGestureDetector &tilt)
    {
        showSliceIntro(display, preset);

        // Static PSRAM scratch (~29KB), not stack -- same convention as
        // OrbitalPresetState/AtomPresetState (see runOrbitalView()'s own `preset` comment).
        static EXT_RAM_BSS_ATTR SliceTable table;
        int64_t buildStartUs = esp_timer_get_time();
        buildSliceTable(preset.resample.n, preset.resample.ell, preset.resample.m, preset.resample.radialCoeff,
                       preset.resample.legendreCoeff, preset.rRef, &table);
        int64_t buildMs = (esp_timer_get_time() - buildStartUs) / 1000;
        ESP_LOGI(kOrbitalViewTag, "slice built in %lldms (n=%d %s=%d m=%d)", buildMs, table.n, kGlyphScriptL,
                 table.ell, table.m);

        auto drawFrame = [&](orb_real_t fade, TiltEvent ev)
        {
            display.waitForFlushDone();
            uint16_t *frameBuf = display.getFrameBuf();
            renderSliceFrame(frameBuf, table, fade);
            drawSliceOverlay(frameBuf, preset, table.extentPm);
            if (ev.phase != TiltPhase::kIdle)
                drawTiltArrow(frameBuf, ev.direction, kAccentColor);
            display.presentFrame();
        };

        bool aborted = false;
        orb_real_t fade = orb_real_t(0);
        for (int i = 0; i < kSliceIntroFrames; i++)
        {
            fade = kSliceIntroFrames > 1 ? orb_real_t(i) / orb_real_t(kSliceIntroFrames - 1) : orb_real_t(1);
            TiltEvent ev = tilt.poll();
            drawFrame(fade, ev);
            if (ev.phase != TiltPhase::kIdle)
            {
                aborted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (!aborted)
        {
            fade = orb_real_t(1);
            int64_t holdStartUs = esp_timer_get_time();
            while (esp_timer_get_time() - holdStartUs < kSliceHoldUs)
            {
                TiltEvent ev = tilt.poll();
                drawFrame(fade, ev);
                if (ev.phase != TiltPhase::kIdle)
                {
                    aborted = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        // Unconditional ease-back to black (and, once this returns, the untouched 3D view) --
        // not itself interruptible, matching runDissectionSequence()'s closing leg.
        for (int i = kSliceFadeOutFrames; i >= 0; i--)
        {
            orb_real_t t = kSliceFadeOutFrames > 0 ? orb_real_t(i) / orb_real_t(kSliceFadeOutFrames) : orb_real_t(0);
            drawFrame(fade * t, TiltEvent{});
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        ESP_LOGI(kOrbitalViewTag, "slice sequence %s", aborted ? "aborted -- movement detected" : "complete");
    }
} // namespace

void renderOrbitalFrame(uint16_t *frameBuf, const OrbitalPresetState &preset, const CameraState &camera,
                        orb_real_t scale, uint32_t frameSalt, uint32_t buzzThreshold)
{
    // Nucleus drawn BEFORE the cloud (matching pc/viewer_common.py's blend order on purpose,
    // see camera.h's renderScene()), so a point landing on the same pixel can alpha-blend
    // over it and dim/hide it near the origin -- the drawProtonMarker() call below redraws it
    // opaque and larger on top, after the cloud, so it's always visible.
    renderScene(frameBuf, preset.points, preset.colors, kOrbitalNumPoints, kProtonColor, camera, scale, frameSalt,
                buzzThreshold);
    drawProtonMarker(frameBuf, kProtonColor, kOrbitalProtonMarkerSize);
    drawText(frameBuf, kTitleTextX, kTitleTextY, preset.title, kTextColor, kFontHuge);
    // The "n=... l=... m=..." numbers below the title, in a smaller font, so the user can see
    // the quantum numbers without having to remember which preset index is which.
    int width = textWidth(preset.orbital_numbers, kFontLarge);
    int height = kFontLarge.height;
    drawText(frameBuf, Display::kDisplayWidth - width, Display::kDisplayHeight - height - 15, preset.orbital_numbers,
             kTextColor, kFontLarge);
    drawScaleBar(frameBuf, scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
}

namespace
{
    /**
     * @brief Like camera.h's flyOver(), but calls renderOrbitalFrame() (nucleus marker
     *        redrawn opaque/enlarged on top, title + quantum numbers, scale bar) instead of
     *        camera.h's own generic per-frame draw -- kept local to this file rather than
     *        changing camera.h's flyOver(): only orbital_view wants the nucleus guaranteed
     *        visible and enlarged.
     */
    void orbitalFlyOver(Display &display, const OrbitalPresetState &preset, CameraState *camera,
                        orb_real_t startScale, orb_real_t endScale, int frames, uint32_t buzzThreshold = 0)
    {
        for (int i = 0; i < frames; i++)
        {
            orb_real_t t = frames > 1 ? orb_real_t(i) / orb_real_t(frames - 1) : orb_real_t(1);
            orb_real_t scale = startScale + (endScale - startScale) * t;

            display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
            renderOrbitalFrame(display.getFrameBuf(), preset, *camera, scale, uint32_t(i), buzzThreshold);
            display.presentFrame();

            stepCamera(camera);
        }
    }
} // namespace

void OrbitalPresetState::load(int index)
{
    const OrbitalDescriptor &d = kOrbitalLibrary[index];
    ESP_LOGI(kOrbitalViewTag, "loading preset %d (%s, n=%d l=%d m=%d)...", index, d.label, d.n, d.ell, d.m);
    int64_t startUs = esp_timer_get_time();

    // Scratch only -- discarded once computeOrbitalLevels() below has consumed them, see
    // buildOrbitalPointCloud()'s docstring. EXT_RAM_BSS_ATTR (PSRAM, see runOrbitalView()'s
    // `preset` below for why): CPU-only access, no DMA involved, so PSRAM's slightly higher
    // access latency is a non-issue here.
    static EXT_RAM_BSS_ATTR orb_real_t psi2[kOrbitalNumPoints];
    static EXT_RAM_BSS_ATTR int8_t signs[kOrbitalNumPoints];
    static EXT_RAM_BSS_ATTR uint8_t levels[kOrbitalNumPoints];

    buildOrbitalPointCloud(d.n, d.ell, d.m, points, psi2, signs, kOrbitalNumPoints, kOrbitalViewSeed,
                           &resample.rng, resample.radialCoeff, resample.legendreCoeff);
    resample.sampler = findOrbitalSampler(d.n, d.ell, d.m);
    assert(resample.sampler != nullptr && "findOrbitalSampler: kOrbitalLibrary entry not found for its own (n,ell,m)");
    resample.n = d.n;
    resample.ell = d.ell;
    resample.m = d.m;
    resample.count = kOrbitalNumPoints;
    resample.cursor = 0;

    computeOrbitalLevels(psi2, kOrbitalNumPoints, levels, resample.psi2Sorted);
    for (int i = 0; i < kOrbitalNumPoints; i++)
        colors[i] = orbitalLevelToColor565(levels[i], signs[i], d.posRgb565, d.negRgb565);

    std::snprintf(title, sizeof(title), "%s", d.label);
    std::snprintf(orbital_numbers, sizeof(orbital_numbers), "n=%d %s=%d m=%d", d.n, kGlyphScriptL, d.ell, d.m);

    OrbitalScale scale = scaleFromRadii(points, kOrbitalNumPoints);
    baseScale = scale.baseScale;
    zoomAmplitude = scale.zoomAmplitude;
    rRef = scale.rRef;

    loadMs = (esp_timer_get_time() - startUs) / 1000;
    ESP_LOGI(kOrbitalViewTag, "%s loaded in %lldms, scale=%.1f", d.label, loadMs, double(baseScale));
}

void OrbitalPresetState::resamplePoints(int count)
{
    for (int i = 0; i < count; i++)
    {
        ResampledOrbitalPoint r = resampleOneOrbitalPoint(&resample, points);
        int level = r.level > kOrbitalColorMaxLevel ? kOrbitalColorMaxLevel : r.level;
        colors[r.index] = orbitalLevelToColor565(level, r.sign, Display::kColorOrbitalRed, Display::kColorOrbitalBlue);
    }
}

void runOrbitalView(Display &display, TiltGestureDetector &tilt)
{
    ESP_LOGI(kOrbitalViewTag, "display ready, %d presets available", kOrbitalLibraryCount);

    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM: this struct alone (points+colors+resample,
    // ~3000 points) is tens of KB, and atom_view.cpp's sibling AtomPresetState (always linked
    // in too, whichever view is actually running) is another ~66KB+ on top -- leaving both in
    // the default internal-RAM .bss starved Display::Display()'s DMA frame-buffer allocation,
    // which aborted at boot. CPU-only access here (rendering reads points every frame, no DMA
    // touches this struct), so PSRAM's slightly higher access latency is a non-issue, unlike
    // the frame buffer itself, which stays in internal DMA-capable RAM (display.cpp's
    // MALLOC_CAP_DMA, untouched).
    static EXT_RAM_BSS_ATTR OrbitalPresetState preset;
    static int presetIndex = -1;
    if (presetIndex < 0) // first-ever call this boot -- later calls (after a menu round-trip)
    {                    // keep whatever preset was last showing
        presetIndex = kOrbitalDefaultPresetIndex;
        preset.load(presetIndex);
    }

    constexpr uint32_t kBuzzThreshold = kHiddenPointsThreshold; // see config/visual_constants.h's comment

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    orbitalFlyOver(display, preset, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale,
                   kOrbitalIntroFrames, kBuzzThreshold);

    FrameStats stats; // FPS + render/prepare moving averages + last-load-ms + free IRAM, see debug/frame_stats.h
    stats.reset();
    stats.lastLoadMs = preset.loadMs;

    int cullCount = int(orb_real_t(kOrbitalNumPoints) * kOrbitalCullFraction);
    if (cullCount < 1)
        cullCount = 1;
    int cullFrameCount = 0;
    uint32_t buzzFrame = 0;
    int zoomExcursionCountdown = nextZoomExcursionCountdown();
    int64_t lastActivityUs = esp_timer_get_time();
    // Caps idle auto-advance to at most one slice sequence per preset before it's forced to
    // jump, mirroring atom_view.cpp's idleDissectedThisElement. Reset whenever a new preset
    // loads, see switchToPreset() below.
    bool idleSlicedThisPreset = false;

    // Shared by the manual Up/Down switch and the idle random jump below.
    auto switchToPreset = [&](int newIndex)
    {
        ESP_LOGI(kOrbitalViewTag, "switching preset %d -> %d", presetIndex, newIndex);
        const OrbitalDescriptor &newD = kOrbitalLibrary[newIndex];
        orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        scrollOrbitalIntro(display, newD.n, newD.ell, newD.m);
        presetIndex = newIndex;
        preset.load(presetIndex);
        stats.lastLoadMs = preset.loadMs;
        idleSlicedThisPreset = false; // fresh preset -- fresh idle-slice budget
        orbitalFlyOver(display, preset, &camera, currentScale, preset.baseScale, kOrbitalSwitchTransitionFrames,
                       kBuzzThreshold);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
        // scrollOrbitalIntro()'s real-time holds plus the flyOver() above spend real
        // wall-clock time without incrementing frameCount; reset the FPS window here so it
        // only ever measures steady-state frames instead of charging that idle time to a
        // later window (see atom_view.cpp's switchToElement() for the same fix).
        stats.reset();
    };

    while (true)
    {
        screenshot_pause::checkpoint(); // see screenshot_pause.h -- lets a screenshot capture happen safely

        TiltEvent tiltEv = tilt.poll();
        if (tiltEv.phase == TiltPhase::kConfirmed)
            lastActivityUs = esp_timer_get_time();

        if (tiltEv.phase == TiltPhase::kConfirmed)
        {
            if (tiltEv.direction == TiltDirection::kLeft)
            {
                ESP_LOGI(kOrbitalViewTag, "tilt LEFT confirmed -- returning to menu");
                return;
            }
            if (tiltEv.direction == TiltDirection::kUp || tiltEv.direction == TiltDirection::kDown)
            {
                int delta = tiltEv.direction == TiltDirection::kDown ? 1 : -1;
                int newIndex = (presetIndex + delta + kOrbitalLibraryCount) % kOrbitalLibraryCount;
                ESP_LOGI(kOrbitalViewTag, "tilt %s confirmed", tiltDirectionName(tiltEv.direction));
                switchToPreset(newIndex);
                continue;
            }
            if (tiltEv.direction == TiltDirection::kRight)
            {
                ESP_LOGI(kOrbitalViewTag, "tilt RIGHT confirmed -- starting slice sequence");
                runSliceSequence(display, preset, tilt);
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                stats.reset(); // see switchToPreset()'s FPS-window comment above
                continue;
            }
        }

        // Idle auto-advance: each idle timeout has a coin-flip chance to slice the current
        // preset instead of jumping, but only once per preset (idleSlicedThisPreset); once
        // that budget is used, idle timeouts always jump -- mirrors atom_view.cpp's
        // idle-dissection. kViewIdleJumpUs is shared with atom_view.cpp.
        if (esp_timer_get_time() - lastActivityUs > kViewIdleJumpUs)
        {
            if (!idleSlicedThisPreset && randomUnit() < orb_real_t(0.5))
            {
                ESP_LOGI(kOrbitalViewTag, "idle 60s+ -- slicing current preset (%s)", preset.title);
                runSliceSequence(display, preset, tilt);
                idleSlicedThisPreset = true;
                zoomAngle = orb_real_t(0);
                zoomExcursionCountdown = nextZoomExcursionCountdown();
                stats.reset(); // see switchToPreset()'s FPS-window comment above
            }
            else
            {
                int newIndex = randomIndexExcluding(presetIndex, kOrbitalLibraryCount);
                ESP_LOGI(kOrbitalViewTag, "idle 60s+ -- jumping to random preset %d", newIndex);
                switchToPreset(newIndex);
            }
            lastActivityUs = esp_timer_get_time();
            continue;
        }

        zoomExcursionCountdown--;
        if (zoomExcursionCountdown <= 0)
        {
            orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
            orb_real_t targetScale =
                preset.baseScale * randomUniform(kZoomExcursionScaleMinFactor, kZoomExcursionScaleMaxFactor);
            orbitalFlyOver(display, preset, &camera, currentScale, targetScale, kOrbitalZoomExcursionEaseFrames,
                           kBuzzThreshold);
            orbitalFlyOver(display, preset, &camera, targetScale, preset.baseScale, kOrbitalZoomExcursionEaseFrames,
                           kBuzzThreshold);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
            // See switchToPreset()'s comment above -- same unmeasured-time issue.
            stats.reset();
            continue;
        }

        cullFrameCount++;
        if (cullFrameCount >= kOrbitalCullRefreshFrames)
        {
            preset.resamplePoints(cullCount);
            cullFrameCount = 0;
        }

        orb_real_t scale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        int64_t tBeforeWait = esp_timer_get_time();
        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        int64_t tAfterWait = esp_timer_get_time();
        renderOrbitalFrame(display.getFrameBuf(), preset, camera, scale, buzzFrame, kBuzzThreshold);
        buzzFrame = buzzFrame < 1000000u ? buzzFrame + 1 : 0;
        if (tiltEv.phase != TiltPhase::kIdle)
            drawTiltArrow(display.getFrameBuf(), tiltEv.direction, kAccentColor);
        display.presentFrame();
        int64_t tAfterPresent = esp_timer_get_time();

        stats.recordFrame(double(tAfterWait - tBeforeWait) / 1000.0, double(tAfterPresent - tAfterWait) / 1000.0);
        stats.maybeLog(kOrbitalViewTag);

        stepCamera(&camera);
        zoomAngle += kOrbitalZoomAngleStep;
        if (zoomAngle >= kTwoPi)
            zoomAngle -= kTwoPi;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
