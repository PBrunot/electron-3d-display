#include "debug/screenshot_batch.h"

#include <algorithm>
#include <cstdio>

#include "views/atom_view.h"
#include "render/camera.h"
#include "render/display.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "render/font.h"
#include "physics/orbital_library.h"
#include "views/orbital_view.h"
#include "render/overlay.h"
#include "debug/screenshot.h"
#include "physics/slater.h"

namespace
{
    constexpr auto kTag = "ss_batch";

    // Same curated selection pc/screenshot.py's DEFAULT_ATOMS uses: one representative
    // element per period/trend inside the Clementi-Raimondi-validated range (Z <= 54, see
    // atom_cloud.h's calibration notes) -- kept identical so the on-device and PC galleries
    // line up.
    constexpr int kBatchAtoms[] = {1, 2, 3, 6, 7, 8, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20,
                                   26, 29, 30, 35, 36, 37, 47, 54};
    constexpr int kBatchAtomCount = sizeof(kBatchAtoms) / sizeof(kBatchAtoms[0]);

    void clearFrame(uint16_t *frameBuf)
    {
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
    }

    void saveShot(const uint16_t *frameBuf, const char *name)
    {
        size_t size = 0;
        if (screenshot::captureAs(frameBuf, name, &size))
            ESP_LOGI(kTag, "saved %s (%u bytes)", name, (unsigned)size);
        else
            ESP_LOGE(kTag, "failed to save %s", name);
    }

    /// Rest camera pose (yaw=0, tilt=kCameraTiltStart, roll=kCameraRollStart), same as every
    /// viewer boots into -- matches pc/screenshot.py's fixed CAMERA used for its stills.
    void captureOrbitals(uint16_t *frameBuf)
    {
        // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM; see orbital_view.cpp's identical
        // static preset for why (this struct alone is tens of KB).
        static EXT_RAM_BSS_ATTR OrbitalPresetState preset;
        CameraState camera;
        for (int i = 0; i < kOrbitalLibraryCount; i++)
        {
            ESP_LOGI(kTag, "orbital %d/%d: %s", i + 1, kOrbitalLibraryCount, kOrbitalLibrary[i].label);
            preset.load(i);

            clearFrame(frameBuf);
            renderScene(frameBuf, preset.points, preset.colors, kOrbitalNumPoints, kProtonColor, camera,
                       preset.baseScale);
            drawText(frameBuf, kTitleTextX, kTitleTextY, preset.title, kTextColor, kFontLarge);
            drawScaleBar(frameBuf, preset.baseScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

            char name[40];
            std::snprintf(name, sizeof(name), "orbital_%s.png", kOrbitalLibrary[i].label);
            saveShot(frameBuf, name);
        }
    }

    void captureAtoms(uint16_t *frameBuf)
    {
        static EXT_RAM_BSS_ATTR AtomPresetState preset;
        CameraState camera;
        for (int i = 0; i < kBatchAtomCount; i++)
        {
            int z = kBatchAtoms[i];
            ESP_LOGI(kTag, "atom %d/%d: Z=%d (%s)", i + 1, kBatchAtomCount, z, elementSymbol(z));
            preset.load(z);

            clearFrame(frameBuf);
            renderSceneGrouped(frameBuf, preset.points, preset.groups, preset.groupCount, kProtonColor, camera,
                               preset.baseScale);
            drawAtomTitle(frameBuf, kTitleTextX, kTitleTextY, z, kTextColor);
            drawScaleBar(frameBuf, preset.baseScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

            char name[40];
            std::snprintf(name, sizeof(name), "atom_%s.png", elementSymbol(z));
            saveShot(frameBuf, name);
        }
    }
} // namespace

void captureAllPresets()
{
    uint16_t *frameBuf = (uint16_t *)heap_caps_malloc(
        Display::kDisplayWidth * Display::kDisplayHeight * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (frameBuf == nullptr)
    {
        ESP_LOGE(kTag, "failed to allocate scratch frame buffer");
        return;
    }

    ESP_LOGI(kTag, "batch capture starting: %d orbitals + %d atoms", kOrbitalLibraryCount, kBatchAtomCount);
    captureOrbitals(frameBuf);
    captureAtoms(frameBuf);
    ESP_LOGI(kTag, "batch capture done");

    heap_caps_free(frameBuf);
}
