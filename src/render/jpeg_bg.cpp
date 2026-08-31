// On-demand JPEG background decoder (TJpgDec, via the ROM), shared by splash_bitmap.cpp and
// orbitals_bitmap.cpp. See jpeg_bg.h for the public contract.
#include "render/jpeg_bg.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "render/display.h"
#include "rom/tjpgd.h"
#include "util/storage_mount.h"

namespace
{
    constexpr auto kTag = "jpeg_bg";
    constexpr auto kMountPoint = "/storage";

    // ChaN's documented minimum working pool for an RGB888 decode is ~3.1KB; some headroom.
    constexpr size_t kWorkPoolSize = 3900;

    /// TJpgDec callback context, threaded through JDEC::device.
    struct DecodeCtx
    {
        FILE *file;
        Display *display;
        int offsetX;
        int offsetY;
        bool sampledBg = false;
        uint16_t bgColor565 = 0;
    };

    /// Streams compressed bytes from the open file. `buf == nullptr` means skip `nbyte` bytes.
    UINT jpegInput(JDEC *jd, BYTE *buf, UINT nbyte)
    {
        auto *ctx = static_cast<DecodeCtx *>(jd->device);
        if (buf == nullptr)
            return fseek(ctx->file, long(nbyte), SEEK_CUR) == 0 ? nbyte : 0;
        return UINT(fread(buf, 1, nbyte, ctx->file));
    }

    /// Converts one decoded MCU block from RGB888 to RGB565, writes it straight into the frame
    /// buffer. Also samples the image's own top-left pixel as the letterbox fill color.
    UINT jpegOutput(JDEC *jd, void *bitmap, JRECT *rect)
    {
        auto *ctx = static_cast<DecodeCtx *>(jd->device);
        const auto *rgb888 = static_cast<const uint8_t *>(bitmap);
        if (!ctx->sampledBg)
        {
            ctx->bgColor565 = Display::packColor565(rgb888[0], rgb888[1], rgb888[2]);
            ctx->sampledBg = true;
        }
        for (int y = rect->top; y <= rect->bottom; y++)
        {
            for (int x = rect->left; x <= rect->right; x++)
            {
                uint8_t r = rgb888[0], g = rgb888[1], b = rgb888[2];
                rgb888 += 3;
                ctx->display->writePx(ctx->offsetX + x, ctx->offsetY + y, Display::packColor565(r, g, b));
            }
        }
        return 1;
    }

    /// Fills the letterbox/pillarbox bars outside the centered image rect.
    void fillBorder(Display &display, int offsetX, int offsetY, int w, int h, uint16_t color)
    {
        for (int y = 0; y < Display::kDisplayHeight; y++)
        {
            if (y < offsetY || y >= offsetY + h)
            {
                for (int x = 0; x < Display::kDisplayWidth; x++)
                    display.writePx(x, y, color);
                continue;
            }
            for (int x = 0; x < offsetX; x++)
                display.writePx(x, y, color);
            for (int x = offsetX + w; x < Display::kDisplayWidth; x++)
                display.writePx(x, y, color);
        }
    }
} // namespace

void drawJpegBackground(Display &display, const char *path, int width, int height)
{
    if (!ensureStorageMounted())
    {
        ESP_LOGE(kTag, "%s mount failed -- %s background will be skipped", kMountPoint, path);
        return;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr)
    {
        ESP_LOGE(kTag, "%s not found -- run the uploadfs step to deploy it", path);
        return;
    }

    int64_t startUs = esp_timer_get_time();

    static uint8_t workPool[kWorkPoolSize]; // static: off the caller's stack
    int offsetX = (Display::kDisplayWidth - width) / 2;
    int offsetY = (Display::kDisplayHeight - height) / 2;
    DecodeCtx ctx{f, &display, offsetX, offsetY};
    JDEC jd;

    JRESULT res = jd_prepare(&jd, jpegInput, workPool, UINT(kWorkPoolSize), &ctx);
    if (res != JDR_OK)
    {
        ESP_LOGE(kTag, "%s: jd_prepare failed (JRESULT %d)", path, int(res));
        fclose(f);
        return;
    }
    if (int(jd.width) != width || int(jd.height) != height)
    {
        ESP_LOGE(kTag, "%s: decoded size %ux%u does not match expected %dx%d",
                 path, unsigned(jd.width), unsigned(jd.height), width, height);
        fclose(f);
        return;
    }

    res = jd_decomp(&jd, jpegOutput, 0);
    fclose(f);
    if (res != JDR_OK)
    {
        ESP_LOGE(kTag, "%s: jd_decomp failed (JRESULT %d)", path, int(res));
        return;
    }

    fillBorder(display, offsetX, offsetY, width, height, ctx.bgColor565);

    int64_t elapsedUs = esp_timer_get_time() - startUs;
    ESP_LOGI(kTag, "%s decoded in %lld us", path, static_cast<long long>(elapsedUs));
}
