#include "orbital_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "equation_bitmap.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_library.h"
#include "overlay.h"
#include "screenshot_pause.h"

static const char *kOrbitalViewTag = "orbital_view";

// ============================================================================================
// Tunable constants
// ============================================================================================

/// Idle time with no confirmed tilt input before runOrbitalView() auto-advances to a random
/// preset. Same value as atom_view.cpp's kIdleJumpUs, kept as a separate constant since
/// nothing shares it across files.
constexpr int64_t kIdleJumpUs = 60'000'000;

// --- Orbital-switch quantum-number reveal (scrollOrbitalIntro()) ---
// Shown before switching to a new preset: a pre-rendered equation image (see
// equation_bitmap.h) sits centered as a dim backdrop, with n/l/m revealed progressively on
// top of it, one stage per short real-time hold.
constexpr int kOrbitalIntroEqX = (Display::kDisplayWidth - kEquationBitmapWidth) / 2;   ///< Horizontal center for the equation backdrop.
constexpr int kOrbitalIntroEqY = (Display::kDisplayHeight - kEquationBitmapHeight) / 2; ///< Vertical center for the equation backdrop.
/// Backdrop color: close to pure white so it reads as "dimmer white" against the foreground
/// text rather than gray that needs squinting to see through the panel/Pepper's-Ghost prism.
constexpr uint16_t kOrbitalIntroEqColor = Display::packColor565(210, 210, 220);
/// Text scale for the n/l/m reveal stages. Fixed at one size (rather than per-stage like
/// atom_view.cpp's pickNameScale()) so the reveal doesn't visually jump bigger->smaller
/// between stages; scale 2 is the largest that keeps every stage's text (including
/// two-digit/negative m) within the panel width.
constexpr int kOrbitalIntroNumberScale = 2;
/// Y position for the reveal text, below the vertically-centered equation backdrop.
constexpr int kOrbitalIntroNumberY = kOrbitalIntroEqY + kEquationBitmapHeight + 20;
/// Hold duration per reveal stage.
constexpr uint32_t kOrbitalIntroStageHoldMs = 550;

// Orbital view runs its zoom/switch animations at 1.5x the shared pacing in camera.h
// (atom_view.cpp keeps the stock pacing), via local scaled copies rather than editing the
// shared constants.
constexpr int kOrbitalIntroFrames = 105;            ///< kIntroFrames (70) * 1.5.
constexpr int kOrbitalSwitchTransitionFrames = 27;  ///< kSwitchTransitionFrames (18) * 1.5.
constexpr int kOrbitalZoomExcursionEaseFrames = 68; ///< round(kZoomExcursionEaseFrames (45) * 1.5).
constexpr orb_real_t kOrbitalZoomAngleStep = kZoomAngleStep / orb_real_t(1.5);

/// Side length in pixels of the solid nucleus marker, drawn each frame. Bigger than
/// camera.h's shared kProtonMarkerSize (3px) so the nucleus reads clearly even where orbital
/// density peaks at the origin; local to this file since atom_view.cpp keeps the shared size.
constexpr int kOrbitalProtonMarkerSize = 7;

// ============================================================================================

namespace
{

    void renderOrbitalIntroStage(Display &display, const char *text)
    {
        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
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

        std::snprintf(stage, sizeof(stage), "n=%d l=%d", n, ell);
        renderOrbitalIntroStage(display, stage);
        vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs));

        std::snprintf(stage, sizeof(stage), "n=%d l=%d m=%d", n, ell, m);
        renderOrbitalIntroStage(display, stage);
        vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs + 500));
    }

    void drawOrbitalProtonMarker(uint16_t *frameBuf, uint16_t color)
    {
        int x0 = Display::kDisplayWidth / 2 - kOrbitalProtonMarkerSize / 2;
        int y0 = Display::kDisplayHeight / 2 - kOrbitalProtonMarkerSize / 2;
        for (int y = y0; y < y0 + kOrbitalProtonMarkerSize; y++)
            for (int x = x0; x < x0 + kOrbitalProtonMarkerSize; x++)
                frameBuf[y * Display::kDisplayWidth + x] = color;
    }

    /**
     * @brief Like camera.h's flyOver(), but redraws the proton marker fully opaque (and larger,
     *        see drawOrbitalProtonMarker() above) on top of every frame after the cloud.
     *
     * The shared flyOver()/renderScene() draw the nucleus BEFORE the cloud (matching
     * pc/viewer_common.py's blend order on purpose, see camera.h), so a point that lands on the
     * same pixel alpha-blends over it and dims/hides it -- likely near the center for any orbital
     * whose density peaks at the origin. Kept local to this file rather than changing camera.h's
     * flyOver(): only orbital_view wants the nucleus guaranteed visible and enlarged.
     */
    template <typename PointT, typename TitleDrawFn>
    void orbitalFlyOver(Display &display, const PointT *points, const uint16_t *colors, int count, TitleDrawFn drawTitle,
                        uint16_t protonColor, uint16_t textColor, uint16_t scaleBarColor, CameraState *camera,
                        orb_real_t startScale, orb_real_t endScale, int frames, uint32_t buzzThreshold = 0)
    {
        for (int i = 0; i < frames; i++)
        {
            orb_real_t t = frames > 1 ? orb_real_t(i) / orb_real_t(frames - 1) : orb_real_t(1);
            orb_real_t scale = startScale + (endScale - startScale) * t;

            display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
            renderScene(display.getFrameBuf(), points, colors, count, protonColor, *camera, scale, uint32_t(i),
                        buzzThreshold);
            drawOrbitalProtonMarker(display.getFrameBuf(), protonColor);
            drawTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, textColor);
            drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", scaleBarColor, textColor);
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
    resample.n = d.n;
    resample.ell = d.ell;
    resample.m = d.m;
    resample.count = kOrbitalNumPoints;
    resample.cursor = 0;

    computeOrbitalLevels(psi2, kOrbitalNumPoints, levels, resample.psi2Sorted);
    for (int i = 0; i < kOrbitalNumPoints; i++)
        colors[i] = orbitalLevelToColor565(levels[i], signs[i], d.posRgb565, d.negRgb565);

    std::snprintf(title, sizeof(title), "%s (n=%d l=%d m=%d)", d.label, d.n, d.ell, d.m);

    OrbitalScale scale = scaleFromRadii(points, kOrbitalNumPoints);
    baseScale = scale.baseScale;
    zoomAmplitude = scale.zoomAmplitude;

    int64_t buildMs = (esp_timer_get_time() - startUs) / 1000;
    ESP_LOGI(kOrbitalViewTag, "%s loaded in %lldms, scale=%.1f", d.label, buildMs, double(baseScale));
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

    constexpr uint16_t kProtonColor = Display::packColor565(255, 0, 0);
    constexpr uint16_t kTextColor = Display::kColorWhite;
    constexpr uint16_t kScaleBarColor = Display::kColorWhite;
    constexpr uint16_t kArrowColor = Display::packColor565(255, 210, 60);
    constexpr uint32_t kBuzzThreshold = kHiddenPointsThreshold; // see camera.h's comment
    constexpr int kFpsUpdateInterval = 50;                      // frame count between FPS log lines

    // preset has static storage duration, so it's odr-usable without capturing --
    // GCC's -Werror rejects capturing a static local as a meaningless no-op capture.
    auto drawTitle = [](uint16_t *frameBuf, int x, int y, uint16_t color)
    {
        drawText(frameBuf, x, y, preset.title, color, kFontLarge);
    };

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    orbitalFlyOver(display, preset.points, preset.colors, kOrbitalNumPoints, drawTitle, kProtonColor, kTextColor,
                   kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale,
                   kOrbitalIntroFrames, kBuzzThreshold);

    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();

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
        orbitalFlyOver(display, preset.points, preset.colors, kOrbitalNumPoints, drawTitle, kProtonColor, kTextColor,
                       kScaleBarColor, &camera, currentScale, preset.baseScale, kOrbitalSwitchTransitionFrames,
                       kBuzzThreshold);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
        // scrollOrbitalIntro()'s real-time holds plus the flyOver() above spend real
        // wall-clock time without incrementing frameCount; reset the FPS window here so it
        // only ever measures steady-state frames instead of charging that idle time to a
        // later window (see atom_view.cpp's switchToElement() for the same fix).
        frameCount = 0;
        fpsWindowStartUs = esp_timer_get_time();
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
                ESP_LOGI(kOrbitalViewTag, "tilt RIGHT confirmed -- no action in orbital view");
        }

        // Idle auto-advance -- see atom_view.cpp's identical kIdleJumpUs comment; reuses
        // switchToPreset()'s exact animation with a random (not adjacent) target.
        if (esp_timer_get_time() - lastActivityUs > kIdleJumpUs)
        {
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
            orbitalFlyOver(display, preset.points, preset.colors, kOrbitalNumPoints, drawTitle, kProtonColor,
                           kTextColor, kScaleBarColor, &camera, currentScale, targetScale, kOrbitalZoomExcursionEaseFrames,
                           kBuzzThreshold);
            orbitalFlyOver(display, preset.points, preset.colors, kOrbitalNumPoints, drawTitle, kProtonColor,
                           kTextColor, kScaleBarColor, &camera, targetScale, preset.baseScale,
                           kOrbitalZoomExcursionEaseFrames, kBuzzThreshold);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
            // See switchToPreset()'s comment above -- same unmeasured-time issue.
            frameCount = 0;
            fpsWindowStartUs = esp_timer_get_time();
            continue;
        }

        cullFrameCount++;
        if (cullFrameCount >= kOrbitalCullRefreshFrames)
        {
            preset.resamplePoints(cullCount);
            cullFrameCount = 0;
        }

        orb_real_t scale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
        renderScene(display.getFrameBuf(), preset.points, preset.colors, kOrbitalNumPoints, kProtonColor, camera,
                    scale, buzzFrame, kBuzzThreshold);
        // Redraw bigger, on top of the cloud -- see orbitalFlyOver()'s docstring above for
        // why (renderScene() alone draws the small shared marker BEFORE the points, so a
        // point landing on the same pixel can dim/hide it).
        drawOrbitalProtonMarker(display.getFrameBuf(), kProtonColor);
        buzzFrame = buzzFrame < 1000000u ? buzzFrame + 1 : 0;
        drawText(display.getFrameBuf(), kTitleTextX, kTitleTextY, preset.title, kTextColor, kFontLarge);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
        if (tiltEv.phase != TiltPhase::kIdle)
            drawTiltArrow(display.getFrameBuf(), tiltEv.direction, kArrowColor);
        display.presentFrame();

        frameCount++;
        if (frameCount >= kFpsUpdateInterval)
        {
            int64_t nowUs = esp_timer_get_time();
            double elapsedS = double(nowUs - fpsWindowStartUs) / 1e6;
            double fps = elapsedS > 0 ? double(frameCount) / elapsedS : 0.0;
            ESP_LOGI(kOrbitalViewTag, "FPS: %.1f", fps);
            fpsWindowStartUs = nowUs;
            frameCount = 0;
        }

        stepCamera(&camera);
        zoomAngle += kOrbitalZoomAngleStep;
        if (zoomAngle >= kTwoPi)
            zoomAngle -= kTwoPi;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
