#include "orbital_view.h"

#include <cmath>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orbital_library.h"
#include "overlay.h"

static const char *kOrbitalViewTag = "orbital_view";

void OrbitalPresetState::load(int index)
{
    const OrbitalDescriptor &d = kOrbitalLibrary[index];
    ESP_LOGI(kOrbitalViewTag, "loading preset %d (%s, n=%d l=%d m=%d)...", index, d.label, d.n, d.ell, d.m);
    int64_t startUs = esp_timer_get_time();

    // Scratch only -- discarded once computeOrbitalLevels() below has consumed them, see
    // buildOrbitalPointCloud()'s docstring.
    static orb_real_t psi2[kOrbitalViewNumPoints];
    static int8_t signs[kOrbitalViewNumPoints];
    static uint8_t levels[kOrbitalViewNumPoints];

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

void runOrbitalView(Display &display)
{
    ESP_LOGI(kOrbitalViewTag, "display ready, %d presets available", kOrbitalLibraryCount);

    static OrbitalPresetState preset;
    preset.load(kOrbitalDefaultPresetIndex);

    constexpr uint16_t kProtonColor = Display::packColor565(255, 0, 0);
    constexpr uint16_t kTextColor = Display::kColorWhite;
    constexpr uint16_t kScaleBarColor = Display::packColor565(210, 210, 210);
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

    while (true)
    {
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
