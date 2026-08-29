/**
 * @file splash_bitmap.cpp
 * @brief Boot splash / chooser-menu background, decoded on demand from data/atomic_cube.jpg.
 *
 * NOT a generated file (unlike the old splash_bitmap.cpp): that version pre-decoded the image
 * offline (tools/splash_gen/render_splash.py) into a ~112.5KB flash-resident RGB565 array
 * (240*240*2 bytes). This version keeps only the ~19KB JPEG in storage and decodes it at
 * runtime with TJpgDec, the JPEG decoder already present in the ESP32/ESP32-S3 ROM (jd_prepare/
 * jd_decomp -- see rom/tjpgd.h; confirmed linked unconditionally for both targets via each
 * chip's esp_rom component, not gated by any menuconfig option -- ESP_ROM_HAS_JPEG_DECODE is a
 * fixed SoC-capability flag, already =y in both boards' sdkconfig, not a switch to flip). Same
 * source code path on both targets.
 *
 * The JPEG is read from data/atomic_cube.jpg via the same "storage" SPIFFS partition every
 * other on-demand flash table already uses (see util/storage_mount.h, physics/hfs_radial.cpp,
 * physics/orbital_library.cpp) -- deployed by the `uploadfs` step already chained onto every
 * flash (tools/extra_script_uploadfs.py), so this costs nothing extra to build or deploy.
 *
 * The ROM decoder's output is fixed at RGB888 (3 bytes/pixel, not configurable from our side --
 * see rom/tjpgd.h's JD_FORMAT comment, baked in when the ROM itself was built), so jpegOutput()
 * below does the RGB888->RGB565 conversion by hand, through the same packColor565() bit formula
 * every other bitmap in this project uses.
 *
 * TJpgDec decodes one MCU block at a time (up to 16x16px here -- data/atomic_cube.jpg is 4:2:0
 * subsampled, confirmed via its SOF0 marker, and 240 is a multiple of 16 so every block is a
 * full 16x16, but jpegOutput() reads the actual JRECT bounds rather than assuming that). Each
 * decoded block is converted and written straight into the Display's own frame buffer via
 * writePx() -- NOT into an intermediate whole-image RGB565 buffer. An earlier version of this
 * file decoded into exactly such a buffer (240*240*2 = 115200 bytes) and cached it permanently
 * so ux/chooser.cpp's menu loop could keep re-blitting it without re-decoding every frame --
 * that buffer overflowed the CYD's internal DRAM at link time (`.dram0.bss` over budget by
 * ~73KB: no PSRAM on that board to redirect it to, unlike physics/orbital_library.cpp's
 * similarly-large sSampler table). The actual fix for the "re-decode every frame" cost turned
 * out to belong in chooser.cpp instead: it now only calls drawSplashScreen() again when the
 * background genuinely needs to reappear (menu (re-)entry), not on every blink poll -- see that
 * file's needsFullRedraw. That makes a 60-90ms decode-and-draw affordable there without needing
 * to cache anything at all here, on either target.
 *
 * RAM usage: the decode's own scratch (workPool below, ROM's JD_SZBUF stream buffer + Huffman/
 * quantization tables + the MCU sample buffer) is ~3.1KB per ChaN's documented minimum for a
 * color image; sized here at kWorkPoolSize with a little headroom, and freed (it's a function-
 * local static, but the point is nothing here scales with image size) as soon as
 * drawSplashScreen() returns. Nothing from a previous call is kept around.
 */
#include "render/splash_bitmap.h"

#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "render/display.h"
#include "rom/tjpgd.h"
#include "util/storage_mount.h"

namespace
{
    constexpr auto kTag = "splash";
    constexpr auto kMountPoint = "/storage"; // for log messages only, see util/storage_mount.h
    constexpr auto kPath = "/storage/atomic_cube.jpg";

    // ChaN's documented minimum working-pool size for a color (RGB888) TJpgDec decode is
    // ~3.1KB (JD_SZBUF's 512-byte stream buffer + Huffman/quantization tables + the MCU sample
    // buffer, all carved out of this pool by jd_prepare -- see rom/tjpgd.h); sized here with
    // some headroom rather than shaved to the exact byte. Local to drawSplashScreen(), not kept
    // around after the one decode that needs it.
    constexpr size_t kWorkPoolSize = 3900;

    /// TJpgDec input/output callback context -- just the open file (for jpegInput()) and the
    /// Display to draw into (for jpegOutput()), threaded through JDEC::device by jd_prepare()'s
    /// last argument.
    struct DecodeCtx
    {
        FILE *file;
        Display *display;
    };

    /// TJpgDec input callback: streams compressed bytes straight from the open file instead of
    /// buffering the whole ~19KB JPEG in RAM first -- ROM's own JD_SZBUF stream buffer (part of
    /// workPool below) already does the staging TJpgDec needs. `buf == nullptr` is TJpgDec's way
    /// of asking to skip `nbyte` bytes (e.g. over a marker segment it doesn't care about)
    /// instead of reading them.
    UINT jpegInput(JDEC *jd, BYTE *buf, UINT nbyte)
    {
        auto *ctx = static_cast<DecodeCtx *>(jd->device);
        if (buf == nullptr)
            return fseek(ctx->file, long(nbyte), SEEK_CUR) == 0 ? nbyte : 0;
        return UINT(fread(buf, 1, nbyte, ctx->file));
    }

    /// TJpgDec output callback: converts one decoded MCU block from the ROM decoder's fixed
    /// RGB888 output to RGB565 and writes it straight into the display's frame buffer via
    /// writePx() -- no intermediate whole-image buffer (see this file's header comment).
    /// Returning 0 would abort the decode early (e.g. if a caller wanted to cancel mid-image) --
    /// always returns 1 here since nothing about this splash's decode is ever cancelled.
    UINT jpegOutput(JDEC *jd, void *bitmap, JRECT *rect)
    {
        auto *ctx = static_cast<DecodeCtx *>(jd->device);
        const auto *rgb888 = static_cast<const uint8_t *>(bitmap);
        for (int y = rect->top; y <= rect->bottom; y++)
        {
            for (int x = rect->left; x <= rect->right; x++)
            {
                uint8_t r = rgb888[0], g = rgb888[1], b = rgb888[2];
                rgb888 += 3;
                ctx->display->writePx(x, y, Display::packColor565(r, g, b));
            }
        }
        return 1;
    }
} // namespace

void drawSplashScreen(Display &display)
{
    if (!ensureStorageMounted())
    {
        ESP_LOGE(kTag, "%s mount failed -- splash/menu background will be skipped",
                 kMountPoint);
        return;
    }

    FILE *f = fopen(kPath, "rb");
    if (f == nullptr)
    {
        ESP_LOGE(kTag, "%s not found -- run `pio run -t uploadfs` to deploy it; "
                       "splash/menu background will be skipped until then",
                 kPath);
        return;
    }

    int64_t startUs = esp_timer_get_time();

    static uint8_t workPool[kWorkPoolSize]; // static: kept off the caller's stack, see
                                             // kWorkPoolSize's comment for why its lifetime
                                             // doesn't need to outlive this call
    DecodeCtx ctx{f, &display};
    JDEC jd;

    JRESULT res = jd_prepare(&jd, jpegInput, workPool, UINT(kWorkPoolSize), &ctx);
    if (res != JDR_OK)
    {
        ESP_LOGE(kTag, "%s: jd_prepare failed (JRESULT %d) -- malformed or unsupported "
                       "JPEG (progressive JPEGs are not supported by this decoder)",
                 kPath, int(res));
        fclose(f);
        return;
    }
    if (int(jd.width) != kSplashBitmapWidth || int(jd.height) != kSplashBitmapHeight)
    {
        ESP_LOGE(kTag, "%s: decoded size %ux%u does not match expected %dx%d",
                 kPath, unsigned(jd.width), unsigned(jd.height),
                 kSplashBitmapWidth, kSplashBitmapHeight);
        fclose(f);
        return;
    }

    res = jd_decomp(&jd, jpegOutput, 0); // scale 0: full resolution, no descaling
    fclose(f);
    if (res != JDR_OK)
    {
        ESP_LOGE(kTag, "%s: jd_decomp failed (JRESULT %d)", kPath, int(res));
        return;
    }

    int64_t elapsedUs = esp_timer_get_time() - startUs;
    ESP_LOGI(kTag, "%s decoded in %lld us (expected ~60000-90000)", kPath,
             static_cast<long long>(elapsedUs));
}
