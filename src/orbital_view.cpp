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

static const char *kOrbitalViewTag = "orbital_view";

// "if no activity, ... same for orbitals" (feedback, 2026-08-17) -- idle threshold for
// OrbitalView's random auto-advance, see runOrbitalView()'s lastActivityUs tracking below.
// Same value as atom_view.cpp's kIdleJumpUs, kept as a separate constant per file since
// nothing else needs to share it.
constexpr int64_t kIdleJumpUs = 60'000'000;

// --- Orbital-switch quantum-number reveal (Right/Left tilt-hold, and the idle jump) ----
//
// "numbers reveal, but in background I want equations -- schroedinger and the decomposed
// psi_real used by our calculation in gray" (feedback, 2026-08-17). The equation image is
// pre-rendered offline (tools/equation_gen/render_equations.py) and blitted as a static
// backdrop -- see equation_bitmap.h's header comment for why an image instead of new font
// glyphs. n/l/m reveal progressively on top of it, one stage per short real-time hold.
namespace {
constexpr int kOrbitalIntroEqX = (Display::kDisplayWidth - kEquationBitmapWidth) / 2;
constexpr int kOrbitalIntroEqY = 140;
// Dim on purpose -- background decoration, not the star of the animation (matches
// atom_view.cpp's kDissectDimColor, same intent: legible but clearly secondary).
constexpr uint16_t kOrbitalIntroEqColor = Display::packColor565(70, 70, 70);
// Scale 2, not 3 (kDissectBigScale's size elsewhere) -- the final stage ("n=X l=Y m=Z",
// m can be negative/2-digit-wide for this library's presets) overflows the 240px width at
// scale 3 (measured up to 282px for "n=3 l=2 m=-2"); scale 2's worst case (188px) leaves
// comfortable margin, and using one fixed scale for all three stages (rather than picking
// per-stage like atom_view.cpp's pickNameScale()) keeps the reveal's size steady instead of
// jumping bigger->smaller when the longest stage appears.
constexpr int kOrbitalIntroNumberScale = 2;
constexpr int kOrbitalIntroNumberY = 50;
constexpr uint32_t kOrbitalIntroStageHoldMs = 550;

void renderOrbitalIntroStage(Display &display, const char *text) {
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

/** Reveal "n=X", then "n=X l=Y", then "n=X l=Y m=Z" (each held kOrbitalIntroStageHoldMs),
 * over the dim equation backdrop -- shown before switching to a new orbital preset, same
 * spot in the flow as atom_view.cpp's scrollElementIntro() before its flyOver(). */
void scrollOrbitalIntro(Display &display, int n, int ell, int m) {
    char stage[24];
    std::snprintf(stage, sizeof(stage), "n=%d", n);
    renderOrbitalIntroStage(display, stage);
    vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs));

    std::snprintf(stage, sizeof(stage), "n=%d l=%d", n, ell);
    renderOrbitalIntroStage(display, stage);
    vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs));

    std::snprintf(stage, sizeof(stage), "n=%d l=%d m=%d", n, ell, m);
    renderOrbitalIntroStage(display, stage);
    vTaskDelay(pdMS_TO_TICKS(kOrbitalIntroStageHoldMs));
}
} // namespace

void OrbitalPresetState::load(int index)
{
    const OrbitalDescriptor &d = kOrbitalLibrary[index];
    ESP_LOGI(kOrbitalViewTag, "loading preset %d (%s, n=%d l=%d m=%d)...", index, d.label, d.n, d.ell, d.m);
    int64_t startUs = esp_timer_get_time();

    // Scratch only -- discarded once computeOrbitalLevels() below has consumed them, see
    // buildOrbitalPointCloud()'s docstring. EXT_RAM_BSS_ATTR (PSRAM, see this file's
    // `preset` below for why): CPU-only access, no DMA involved, so PSRAM's slightly
    // higher access latency is a non-issue here.
    static EXT_RAM_BSS_ATTR orb_real_t psi2[kOrbitalViewNumPoints];
    static EXT_RAM_BSS_ATTR int8_t signs[kOrbitalViewNumPoints];
    static EXT_RAM_BSS_ATTR uint8_t levels[kOrbitalViewNumPoints];

    buildOrbitalPointCloud(d.n, d.ell, d.m, points, psi2, signs, kOrbitalViewNumPoints, kOrbitalViewSeed,
                           &resample.rng, resample.radialCoeff, resample.legendreCoeff);
    resample.sampler = findOrbitalSampler(d.n, d.ell, d.m);
    resample.n = d.n;
    resample.ell = d.ell;
    resample.m = d.m;
    resample.count = kOrbitalViewNumPoints;
    resample.cursor = 0;

    computeOrbitalLevels(psi2, kOrbitalViewNumPoints, levels, resample.psi2Sorted);
    for (int i = 0; i < kOrbitalViewNumPoints; i++)
        colors[i] = orbitalLevelToColor565(levels[i], signs[i]);

    std::snprintf(title, sizeof(title), "%s (n=%d l=%d m=%d)", d.label, d.n, d.ell, d.m);

    OrbitalScale scale = scaleFromRadii(points, kOrbitalViewNumPoints);
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
        colors[r.index] = orbitalLevelToColor565(level, r.sign);
    }
}

void runOrbitalView(Display &display, TiltGestureDetector &tilt)
{
    ESP_LOGI(kOrbitalViewTag, "display ready, %d presets available", kOrbitalLibraryCount);

    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM: this struct alone (points+colors+
    // resample, ~3000 points) is tens of KB, and atom_view.cpp's sibling AtomPresetState
    // (always linked in too, whichever view is actually running) is another ~66KB+ on top
    // -- on real hardware, leaving both in the default internal-RAM .bss starved
    // Display::Display()'s 112.5KB DMA frame-buffer allocation, which aborted at boot
    // (twice -- see atom_view.cpp's compactDissectLevelInPlace() docstring for the first,
    // smaller-scoped fix that wasn't enough on its own). CONFIG_SPIRAM_ALLOW_BSS_SEG_
    // EXTERNAL_MEMORY is already enabled in this project's sdkconfig (confirmed via
    // .pio/build/*/config/sdkconfig.json), it just wasn't being used anywhere -- CPU-only
    // access here (rendering reads points every frame, no DMA touches this struct), so
    // PSRAM's slightly higher access latency is a non-issue, unlike the frame buffer
    // itself, which stays in internal DMA-capable RAM (display.cpp's MALLOC_CAP_DMA,
    // untouched).
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
    constexpr uint32_t kBuzzThreshold = uint32_t(kOrbitalBuzzFraction * orb_real_t(65536));

    // preset has static storage duration, so it's odr-usable without capturing --
    // GCC's -Werror rejects capturing a static local as a meaningless no-op capture.
    auto drawTitle = [](uint16_t *frameBuf, int x, int y, uint16_t color)
    {
        drawText(frameBuf, x, y, preset.title, color, kFontLarge);
    };

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    flyOver(display, preset.points, preset.colors, kOrbitalViewNumPoints, drawTitle, kProtonColor, kTextColor,
            kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames,
            kBuzzThreshold);

    constexpr int kFpsUpdateInterval = 50;
    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();

    int cullCount = int(orb_real_t(kOrbitalViewNumPoints) * kOrbitalCullFraction);
    if (cullCount < 1)
        cullCount = 1;
    int cullFrameCount = 0;
    uint32_t buzzFrame = 0;
    int zoomExcursionCountdown = nextZoomExcursionCountdown();
    int64_t lastActivityUs = esp_timer_get_time();

    // Shared by the manual Right/Left switch and the idle random jump below -- was only
    // written once (for Right/Left) before the idle timer needed the identical sequence
    // with a random target instead of an adjacent one.
    auto switchToPreset = [&](int newIndex) {
        ESP_LOGI(kOrbitalViewTag, "switching preset %d -> %d", presetIndex, newIndex);
        const OrbitalDescriptor &newD = kOrbitalLibrary[newIndex];
        orb_real_t currentScale = preset.baseScale + preset.zoomAmplitude * std::sin(zoomAngle);
        scrollOrbitalIntro(display, newD.n, newD.ell, newD.m);
        presetIndex = newIndex;
        preset.load(presetIndex);
        flyOver(display, preset.points, preset.colors, kOrbitalViewNumPoints, drawTitle, kProtonColor, kTextColor,
                kScaleBarColor, &camera, currentScale, preset.baseScale, kSwitchTransitionFrames, kBuzzThreshold);
        zoomAngle = orb_real_t(0);
        zoomExcursionCountdown = nextZoomExcursionCountdown();
    };

    while (true)
    {
        TiltEvent tiltEv = tilt.poll();
        if (tiltEv.phase == TiltPhase::kConfirmed || tiltEv.phase == TiltPhase::kConfirmedLong)
            lastActivityUs = esp_timer_get_time();

        if (tiltEv.phase == TiltPhase::kConfirmed)
        {
            if (tiltEv.direction == TiltDirection::kUp)
            {
                ESP_LOGI(kOrbitalViewTag, "tilt UP confirmed -- returning to menu");
                return;
            }
            if (tiltEv.direction == TiltDirection::kRight || tiltEv.direction == TiltDirection::kLeft)
            {
                int delta = tiltEv.direction == TiltDirection::kRight ? 1 : -1;
                int newIndex = (presetIndex + delta + kOrbitalLibraryCount) % kOrbitalLibraryCount;
                ESP_LOGI(kOrbitalViewTag, "tilt %s confirmed", tiltDirectionName(tiltEv.direction));
                switchToPreset(newIndex);
                continue;
            }
            if (tiltEv.direction == TiltDirection::kDown)
                ESP_LOGI(kOrbitalViewTag, "tilt DOWN confirmed -- no action in orbital view");
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
            flyOver(display, preset.points, preset.colors, kOrbitalViewNumPoints, drawTitle, kProtonColor,
                    kTextColor, kScaleBarColor, &camera, currentScale, targetScale, kZoomExcursionEaseFrames,
                    kBuzzThreshold);
            flyOver(display, preset.points, preset.colors, kOrbitalViewNumPoints, drawTitle, kProtonColor,
                    kTextColor, kScaleBarColor, &camera, targetScale, preset.baseScale, kZoomExcursionEaseFrames,
                    kBuzzThreshold);
            zoomAngle = orb_real_t(0);
            zoomExcursionCountdown = nextZoomExcursionCountdown();
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
        renderScene(display.getFrameBuf(), preset.points, preset.colors, kOrbitalViewNumPoints, kProtonColor, camera,
                    scale, buzzFrame, kBuzzThreshold);
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
        zoomAngle += kZoomAngleStep;
        if (zoomAngle >= kTwoPi)
            zoomAngle -= kTwoPi;

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
