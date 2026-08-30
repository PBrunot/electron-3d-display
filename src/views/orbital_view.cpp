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
#include "sdkconfig.h" // CONFIG_IDF_TARGET_ESP32
#include "debug/frame_stats.h"
#include "debug/screenshot_pause.h"
#include "config/visual_constants.h" // kViewIdleJumpUs, kOrbitalIntro*, kOrbitalProtonMarkerSize, etc.

static const char *kOrbitalViewTag = "orbital_view";

namespace
{

    void renderOrbitalIntroStage(Display &display, const char *text)
    {
        display.waitForFlushDone();
        display.clearScreen();
        drawEquationBackdrop(display, kOrbitalIntroEqX, kOrbitalIntroEqY, kOrbitalIntroEqColor);
        int width = textWidthScaled(text, kFontLarge, kOrbitalIntroNumberScale);
        int x = (Display::kDisplayWidth - width) / 2;
        drawTextScaled(display, x, kOrbitalIntroNumberY, text, Display::kColorWhite, kFontLarge,
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

void renderOrbitalFrame(Display &display, const OrbitalPresetState &preset, const CameraState &camera,
                        orb_real_t scale, uint32_t frameSalt, uint32_t buzzThreshold)
{
    // Nucleus drawn BEFORE the cloud (matching pc/viewer_common.py's blend order on purpose,
    // see camera.h's renderScene()), so a point landing on the same pixel can alpha-blend
    // over it and dim/hide it near the origin -- the drawProtonMarker() call below redraws it
    // opaque and larger on top, after the cloud, so it's always visible.
    renderScene(display, preset.points, preset.colors, kOrbitalNumPoints, kProtonColor, camera, scale, frameSalt,
                buzzThreshold);
    drawProtonMarker(display, kProtonColor, kOrbitalProtonMarkerSize);
    drawText(display, kTitleTextX, kTitleTextY, preset.title, kTextColor, kFontHuge);
    // The "n=... l=... m=..." numbers below the title, in a smaller font, so the user can see
    // the quantum numbers without having to remember which preset index is which.
    int width = textWidth(preset.orbital_numbers, kFontLarge);
    int height = kFontLarge.height;
    drawText(display, Display::kDisplayWidth - width, Display::kDisplayHeight - height - 15, preset.orbital_numbers,
             kTextColor, kFontLarge);
    drawScaleBar(display, scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
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
            renderOrbitalFrame(display, preset, *camera, scale, uint32_t(i), buzzThreshold);
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

    formatOrbitalTitle(title, sizeof(title), d.n, d.ell, d.m);
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
        }

        // Idle auto-advance: jump to a random preset after kViewIdleJumpUs of no tilt input.
        // kViewIdleJumpUs is shared with atom_view.cpp.
        if (esp_timer_get_time() - lastActivityUs > kViewIdleJumpUs)
        {
#if CONFIG_IDF_TARGET_ESP32
            // CYD has no tilt input to leave this view and reach chooser.cpp's own idle
            // orbital/element coin-flip (see kViewCrossSwitchProbability's doc comment) --
            // main.cpp's CYD boot path loops this view and runAtomView() directly, so
            // returning here is what lets idle browsing reach the element viewer at all.
            if (randomUnit() < kViewCrossSwitchProbability)
            {
                ESP_LOGI(kOrbitalViewTag, "CYD: idle 60s+ -- crossing over to element viewer");
                return;
            }
#endif
            int newIndex = randomIndexExcluding(presetIndex, kOrbitalLibraryCount);
            ESP_LOGI(kOrbitalViewTag, "idle 60s+ -- jumping to random preset %d", newIndex);
            switchToPreset(newIndex);
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
        renderOrbitalFrame(display, preset, camera, scale, buzzFrame, kBuzzThreshold);
        buzzFrame = buzzFrame < 1000000u ? buzzFrame + 1 : 0;
        if (tiltEv.phase != TiltPhase::kIdle)
            drawTiltArrow(display, tiltEv.direction, kAccentColor);
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
