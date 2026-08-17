#include "atom_view.h"

#include <cmath>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "overlay.h"
#include "slater.h"

static const char* kAtomViewTag = "atom_view";

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

void drawAtomTitle(uint16_t* frameBuf, int x, int y, int z, const ElectronConfig& config, uint16_t textColor) {
    int cursorX = x, cursorY = y;

    auto drawSegment = [&](const char* segment, uint16_t color) {
        int segWidth = textWidth(segment);
        if (cursorX > x && cursorX + segWidth > Display::kDisplayWidth) {
            cursorX = x;
            cursorY += kFontAdvanceY;
        }
        cursorX = drawText(frameBuf, cursorX, cursorY, segment, color);
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

void runAtomView(Display& display) {
    ESP_LOGI(kAtomViewTag, "display ready, Z=1..%d available", kMaxZ);

    static AtomPresetState preset;
    preset.load(kAtomViewDefaultZ);

    constexpr uint16_t kProtonColor = Display::packColor565(255, 0, 0);
    constexpr uint16_t kTextColor = Display::kColorWhite;
    constexpr uint16_t kScaleBarColor = Display::packColor565(210, 210, 210);

    // preset has static storage duration, so it's odr-usable without capturing --
    // GCC's -Werror rejects capturing a static local as a meaningless no-op capture.
    auto drawTitle = [](uint16_t* frameBuf, int x, int y, uint16_t color) {
        drawAtomTitle(frameBuf, x, y, preset.z, preset.config, color);
    };

    CameraState camera;
    orb_real_t zoomAngle = orb_real_t(0);

    flyOver(display, preset.points, preset.colors, kAtomViewNumPoints, drawTitle, kProtonColor, kTextColor,
            kScaleBarColor, &camera, preset.baseScale * kIntroStartScaleFactor, preset.baseScale, kIntroFrames);

    constexpr int kFpsUpdateInterval = 50;
    char fpsText[16] = "FPS: --";
    int frameCount = 0;
    int64_t fpsWindowStartUs = esp_timer_get_time();
    int zoomExcursionCountdown = nextZoomExcursionCountdown();

    while (true) {
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
        drawAtomTitle(display.getFrameBuf(), kTitleTextX, kTitleTextY, preset.z, preset.config, kTextColor);
        drawText(display.getFrameBuf(), kFpsTextX, kFpsTextY, fpsText, kTextColor);
        drawScaleBar(display.getFrameBuf(), scale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);
        display.presentFrame();

        frameCount++;
        if (frameCount >= kFpsUpdateInterval) {
            int64_t nowUs = esp_timer_get_time();
            double elapsedS = double(nowUs - fpsWindowStartUs) / 1e6;
            double fps = elapsedS > 0 ? double(frameCount) / elapsedS : 0.0;
            std::snprintf(fpsText, sizeof(fpsText), "FPS: %.1f", fps);
            ESP_LOGI(kAtomViewTag, "%s", fpsText);
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
