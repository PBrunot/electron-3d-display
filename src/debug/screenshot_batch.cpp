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
            renderOrbitalFrame(frameBuf, preset, camera, preset.baseScale, /*frameSalt=*/0, /*buzzThreshold=*/0);

            char name[40];
            std::snprintf(name, sizeof(name), "orbital_%s.png", kOrbitalLibrary[i].label);
            saveShot(frameBuf, name);
        }
    }

    void captureAtoms(uint16_t *frameBuf, AtomPresetState &preset)
    {
        CameraState camera;
        for (int i = 0; i < kBatchAtomCount; i++)
        {
            int z = kBatchAtoms[i];
            ESP_LOGI(kTag, "atom %d/%d: Z=%d (%s)", i + 1, kBatchAtomCount, z, elementSymbol(z));
            preset.load(z);

            clearFrame(frameBuf);
            renderAtomFrame(frameBuf, preset, camera, preset.baseScale, /*frameSalt=*/0, /*buzzThreshold=*/0);

            char name[40];
            std::snprintf(name, sizeof(name), "atom_%s.png", elementSymbol(z));
            saveShot(frameBuf, name);
        }
    }

    /// A couple of representative stills from the Right-tilt-hold shell-dissection sequence
    /// (atom_view.cpp's runDissectionSequence()), which otherwise has no screenshot
    /// coverage: level 1 (full atom, outermost shell highlighted) and the last level (bare
    /// innermost-shell core alone). Iron (Z=26, [Ar] 3d6 4s2) has enough distinct subshells
    /// (7) to make the peeling obvious between the two. Takes `preset` by reference rather
    /// than owning its own static instance -- callers pass captureAtoms()'s, so there's only
    /// ever one ~94KB AtomPresetState reserved for the whole batch capture.
    void captureFeDissection(uint16_t *frameBuf, AtomPresetState &preset)
    {
        constexpr int kFeZ = 26;
        CameraState camera;
        ESP_LOGI(kTag, "Fe dissection: loading Z=%d", kFeZ);
        preset.load(kFeZ);

        DissectionEntry plan[kMaxConfigSubshells];
        int planCount = subshellDissectionPlan(preset.points, preset.ranges, preset.rangeCount, plan);

        for (int level : {1, planCount})
        {
            clearFrame(frameBuf);
            renderAtomDissectFrame(frameBuf, preset, camera, level);
            char name[40];
            std::snprintf(name, sizeof(name), "atom_Fe_dissect_%d.png", level);
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

    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM; see orbital_view.cpp's identical static
    // preset for why (this struct alone is tens of KB). Shared by captureAtoms() and
    // captureFeDissection() so there's only one such reservation for the whole batch.
    static EXT_RAM_BSS_ATTR AtomPresetState atomPreset;

    ESP_LOGI(kTag, "batch capture starting: %d orbitals + %d atoms", kOrbitalLibraryCount, kBatchAtomCount);
    captureOrbitals(frameBuf);
    captureAtoms(frameBuf, atomPreset);
    captureFeDissection(frameBuf, atomPreset);
    ESP_LOGI(kTag, "batch capture done");

    heap_caps_free(frameBuf);
}
