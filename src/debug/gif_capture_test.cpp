#include "debug/gif_capture_test.h"

#include <cstdio>

#include "views/atom_view.h"
#include "render/camera.h"
#include "debug/screenshot.h"
#include "physics/slater.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace
{
    constexpr auto kTag = "gif_test";
    constexpr int kGifZ = 26; // Fe -- multi-shell, same pick as benchmark_test.cpp/screenshot_batch.cpp
    constexpr int kGifFrameCount = 96;
} // namespace

void runGifCaptureTest(Display &display)
{
    // screenshot::init() is already called by main.cpp's startScreenshotConsole() before this
    // runs -- calling it again here would just fail the second esp_vfs_spiffs_register().

    // Deliberately its OWN private backing storage, NOT physics/view_steady_arena.h's shared
    // arena the live orbital_view.cpp/atom_view.cpp views use -- see
    // debug/screenshot_batch.cpp's captureOrbitals() for why that separation matters.
    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM; same reasoning as benchmark_test.cpp's/
    // screenshot_batch.cpp's own AtomPresetState statics.
    static EXT_RAM_BSS_ATTR AtomPoint points[kAtomNumPoints];
    static AtomPresetState preset;
    preset.points = points;
    preset.load(kGifZ);

    constexpr size_t kBufBytes = size_t(Display::kDisplayWidth) * Display::kDisplayHeight * sizeof(uint16_t);
    uint16_t *pixelBuf = (uint16_t *)heap_caps_malloc(kBufBytes, MALLOC_CAP_SPIRAM);
    if (pixelBuf == nullptr)
    {
        ESP_LOGE(kTag, "failed to allocate pixel scratch buffer");
        return;
    }

    CameraState camera;
    display.clearScreen();
    ESP_LOGI(kTag, "GIF,START,%s,Z,%d,frames,%d", elementSymbol(kGifZ), kGifZ, kGifFrameCount);
    for (int i = 0; i < kGifFrameCount; i++)
    {
        display.waitForFlushDone();
        renderAtomFrame(display, preset, camera, preset.baseScale, uint32_t(i), kHiddenPointsThreshold);

        // Read the just-rendered frame out and encode it BEFORE presentFrame() kicks off this
        // frame's DMA, so there's no concurrent hardware/software read of the same blocks.
        display.readAllPixels(pixelBuf);
        char name[32];
        std::snprintf(name, sizeof(name), "gif_%03d.png", i);
        if (!screenshot::captureAs(pixelBuf, name, nullptr))
            ESP_LOGE(kTag, "failed to save %s", name);
        else if (i % 10 == 0)
            ESP_LOGI(kTag, "GIF,FRAME,%d/%d", i, kGifFrameCount);

        display.presentFrame();
        stepCamera(&camera);
    }

    heap_caps_free(pixelBuf);
    ESP_LOGI(kTag, "GIF,DONE");
}
