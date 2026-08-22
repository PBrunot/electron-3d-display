/**
 * @file display.h
 * @brief ESP-IDF `esp_lcd` SPI bring-up and framebuffer management.
 *
 * Two targets share this file at compile time (CONFIG_IDF_TARGET_ESP32 branches below):
 * - Waveshare ESP32-S3-LCD-1.3: ST7789V2 240x240 panel, PSRAM available.
 * - CYD (ESP32-2432S028R, "Cheap Yellow Display"): ILI9341 240x320 panel, plain ESP32
 *   (Xtensa LX6), no PSRAM, internal SRAM fragmented into several non-contiguous heap
 *   regions at boot (see CYD-branch.md) -- driving the block-based frame buffer below.
 *
 * All pixel colors in this project MUST be produced via packColor565() (or the palette
 * constants below, which are already packed) and written through writePx()/blit() -- never
 * by poking a raw pointer. Storage byte order/row order is an internal Display concern (see
 * storageColor()/physicalRow()), not something callers need to know about.
 *
 * Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/spi_lcd.html
 */
#pragma once

#include <cstdint>

#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h" // CONFIG_IDF_TARGET_ESP32

namespace detail
{
    /**
     * @brief RGB565 packing, factored out of Display so it can seed the constexpr palette below.
     *
     * A class's own static constexpr member functions aren't usable in a constant expression
     * until the class body finishes parsing, so Display::packColor565() can't initialize the
     * kColor* constants declared in the same class. This free function can, since it doesn't
     * depend on Display being complete. Display::packColor565() just forwards to it.
     */
    constexpr uint16_t packColor565Impl(uint8_t r, uint8_t g, uint8_t b)
    {
        return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
} // namespace detail

class Display
{
public:
    /// Test seam: wraps an already-constructed panel/framebuffer without touching hardware.
    /// Treats `frameBuf` as the whole logical buffer in a single block, matching the shape
    /// the real constructor produces when the heap happens to satisfy it in one allocation.
    Display(esp_lcd_panel_handle_t panel, uint16_t *frameBuf);

    /**
     * @brief Bring up backlight, SPI bus, LCD panel, and a DMA-capable frame buffer.
     * @note Aborts via ESP_ERROR_CHECK/abort() on any failure.
     */
    Display();
    ~Display();

    Display(const Display &) = delete;
    Display(Display &&) = delete;
    Display &operator=(const Display &) = delete;
    Display &operator=(Display &&) = delete;

    /**
     * @brief Queue every block's DMA transfer to the panel and return -- does NOT wait for
     *        transmission to finish.
     *
     * Every block already holds pixels in the exact byte order and physical row order the
     * panel needs (see physicalRow()/storageColor() below), so this is a direct DMA transfer
     * per block, no scratch buffer and no per-pixel transform at present time. Fire-and-forget:
     * blocks are queued back to back (bounded by io_config.trans_queue_depth in display.cpp;
     * esp_lcd_panel_draw_bitmap() itself blocks a caller that outruns that queue, so this never
     * overflows it, just stops returning early in that edge case). Callers are free to do CPU
     * work that doesn't touch the frame buffer before their next waitForFlushDone() call -- that
     * work overlaps the SPI transfer instead of strictly following it. The contract is: call
     * waitForFlushDone() before writing into the frame buffer again.
     */
    void presentFrame();

    /**
     * @brief Write one pixel into the logical frame buffer.
     *
     * Bounds-checked: silently a no-op if (x, y) is outside [0, kDisplayWidth) x
     * [0, kDisplayHeight) -- callers don't need to replicate that check themselves. This is
     * the ONLY way external code touches pixel storage; there is no pointer accessor, so
     * Display stays free to change the buffer's layout (disjoint memory, axis flips, per-byte
     * order, ...) without touching any call site.
     */
    inline void writePx(int x, int y, uint16_t color565)
    {
        if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight)
            return;
        int physY = physicalRow(y);
        blocks[physY / rowsPerBlock][(physY % rowsPerBlock) * kDisplayWidth + x] = storageColor(color565);
    }

    /// Read one pixel back from the logical frame buffer; kColorBlack if (x, y) is out of
    /// bounds. Needed alongside writePx() for read-modify-write ops (fade, alpha blend).
    inline uint16_t readPx(int x, int y) const
    {
        if (x < 0 || x >= kDisplayWidth || y < 0 || y >= kDisplayHeight)
            return kColorBlack;
        int physY = physicalRow(y);
        return storageColor(blocks[physY / rowsPerBlock][(physY % rowsPerBlock) * kDisplayWidth + x]);
    }

    /// Fill the whole frame buffer with black. Implemented as a direct per-block scan (not a
    /// writePx() loop) to stay cheap on every full-screen redraw.
    void clearScreen();

    /// Fade every pixel toward black by keepQ8/256 (see Display::fadeColor565()). Direct scan
    /// over the internal blocks, same reasoning as clearScreen() -- this runs every rendered
    /// frame. See camera.h's kPersistenceKeepQ8.
    void fade(uint16_t keepQ8);

    /**
     * @brief Blit a `srcWidth` x `srcHeight` RGB565 bitmap into the frame buffer, top-left at
     *        (x, y), clipped against the display bounds.
     * @note Row-wise copy internally, not a per-pixel writePx() loop -- used for whole-image
     *       backgrounds (splash screen, chooser menu) redrawn every frame. `src` must be in
     *       standard-layout RGB565 (as produced by packColor565()), matching every generated
     *       bitmap in this project.
     */
    void blit(int x, int y, const uint16_t *src, int srcWidth, int srcHeight);

    /**
     * @brief Copy every pixel out into a caller-provided, row-major, standard-layout RGB565
     *        buffer (`dest` must hold at least kDisplayWidth*kDisplayHeight elements).
     * @note For callers that need the whole frame as one contiguous array (PNG screenshot
     *       encoding) -- everything else should use writePx()/readPx()/blit() directly instead
     *       of reconstructing a flat buffer.
     */
    void readAllPixels(uint16_t *dest) const;

    /**
     * @brief Block until every block queued by the most recent presentFrame() has actually
     *        been transmitted.
     *
     * Drains one completion per outstanding block off a counting semaphore (given once per
     * block by onColorTransDone(), the DMA-complete ISR callback). If presentFrame() already
     * fully finished by the time this is called, there is nothing left to drain and this
     * returns immediately. Call this before overwriting the frame buffer for a new frame.
     */
    auto waitForFlushDone() -> bool;

    /**
     * @brief For a caller that is NOT the render loop's own present/wait pair (currently only
     * screenshot_console.cpp's single-shot capture, which calls readAllPixels() directly while
     * the render loop is paused at a screenshot_pause checkpoint): waits out any DMA still in
     * flight exactly like waitForFlushDone(), but re-gives one completion afterward so the
     * render loop's own next waitForFlushDone() call, once unpaused, still finds "ready" instead
     * of blocking forever on a signal this call already consumed.
     * @note Only safe while the render loop task is actually quiesced (e.g. inside a confirmed
     * screenshot_pause::requestPause() window) -- otherwise this races the render loop's own
     * waitForFlushDone() for the same semaphore.
     */
    auto syncForExternalRead() -> bool;

    /**
     * @brief Pack an (r, g, b) triple (each 0-255) into RGB565 (R5-G6-B5).
     *
     * Standard bit layout, no per-project compensation -- the panel's esp_lcd config
     * (rgb_ele_order / data_endian, set in the constructor) plus storageColor() below are what
     * make this correct on both targets' hardware. Every color in this project must be packed
     * through this function rather than built by hand.
     */
    static inline constexpr uint16_t packColor565(uint8_t r, uint8_t g, uint8_t b)
    {
        return detail::packColor565Impl(r, g, b);
    }

    /// Inverse of packColor565(): expands a packed pixel back to 8-bit-per-channel (r, g, b).
    static void unpackColor565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b);

    /**
     * @brief Alpha-blend `target` into `base` by `alphaQ8`/256 (0 = unchanged, 256 = replaced).
     *
     * Used so overlapping points converge toward full brightness instead of the last-drawn
     * point overwriting whatever was underneath. See camera.h's kElectronAlphaQ8.
     */
    static uint16_t blendColor565(uint16_t base, uint16_t target, uint16_t alphaQ8);

    /**
     * @brief Scale `c`'s channels toward black by `keepQ8`/256 (256 = unchanged, 0 = black).
     *
     * Used to fade the whole frame buffer toward black each frame instead of hard-clearing
     * it, producing motion trails. See Display::fade() / camera.h's kPersistenceKeepQ8.
     */
    static uint16_t fadeColor565(uint16_t c, uint16_t keepQ8);

    static constexpr uint16_t kColorBlack = 0x0000;
    static constexpr uint16_t kColorWhite = 0xFFFF;

    // Named palette matching Arduino_GFX's RGB565_* reference colors, computed via
    // detail::packColor565Impl() (not the packColor565() member -- see its forward
    // declaration above) so they always track this panel's actual packing.
    static constexpr uint16_t kColorNavy = detail::packColor565Impl(0, 0, 123);
    static constexpr uint16_t kColorDarkGreen = detail::packColor565Impl(0, 125, 0);
    static constexpr uint16_t kColorDarkCyan = detail::packColor565Impl(0, 125, 123);
    static constexpr uint16_t kColorMaroon = detail::packColor565Impl(123, 0, 0);
    static constexpr uint16_t kColorPurple = detail::packColor565Impl(123, 0, 123);
    static constexpr uint16_t kColorOlive = detail::packColor565Impl(123, 125, 0);
    static constexpr uint16_t kColorLightGrey = detail::packColor565Impl(198, 195, 198);
    static constexpr uint16_t kColorDarkGrey = detail::packColor565Impl(123, 125, 123);
    static constexpr uint16_t kColorBlue = detail::packColor565Impl(0, 0, 255);
    static constexpr uint16_t kColorGreen = detail::packColor565Impl(0, 255, 0);
    static constexpr uint16_t kColorCyan = detail::packColor565Impl(0, 255, 255);
    static constexpr uint16_t kColorRed = detail::packColor565Impl(255, 0, 0);
    static constexpr uint16_t kColorMagenta = detail::packColor565Impl(255, 0, 255);
    static constexpr uint16_t kColorYellow = detail::packColor565Impl(255, 255, 0);
    static constexpr uint16_t kColorOrange = detail::packColor565Impl(255, 165, 0);
    static constexpr uint16_t kColorGreenYellow = detail::packColor565Impl(173, 255, 41);
    static constexpr uint16_t kColorPaleRed = detail::packColor565Impl(255, 130, 198);
    static constexpr uint16_t kColorOrbitalBlue = detail::packColor565Impl(40, 80, 210);
    static constexpr uint16_t kColorOrbitalRed = detail::packColor565Impl(210, 40, 40);

    /// Panel resolution in pixels; drives frame buffer size and SPI transfer size.
#if CONFIG_IDF_TARGET_ESP32
    // CYD (ESP32-2432S028R): ILI9341, 240x320 native/portrait. Logical canvas is SMALLER than
    // the physical panel (kCydPanelWidth/Height below) and centered in a black letterbox --
    // hardware-measured on this exact unit (see CYD-branch.md's boot log): total DMA-capable
    // internal SRAM free by the time Display() allocates cannot cover a full 240x320 buffer
    // (153600 bytes) even split across every fragmented region down to 1 row/block -- the
    // block-splitting allocator in the constructor aborts. 192x192 (73728 bytes) fits inside
    // this unit's single largest free DMA block (~108KB) with margin to spare for the rest of
    // the app. TEMPORARY: revisit once the real fix (freeing up more DMA-capable SRAM
    // elsewhere, or a smaller pixel format) lets the full panel resolution actually boot.
    static constexpr int kDisplayWidth = 192;
    static constexpr int kDisplayHeight = 192;

    /// Physical ILI9341 panel resolution -- always bigger than the logical canvas above on
    /// this target. presentFrame() centers the logical canvas inside this via kCydOffsetX/Y.
    static constexpr int kCydPanelWidth = 240;
    static constexpr int kCydPanelHeight = 320;
    static constexpr int kCydOffsetX = (kCydPanelWidth - kDisplayWidth) / 2;
    static constexpr int kCydOffsetY = (kCydPanelHeight - kDisplayHeight) / 2;
#else
    static constexpr int kDisplayWidth = 240;
    static constexpr int kDisplayHeight = 240;
#endif

    /// Plain function pointer required by on_color_trans_done (no implicit `this`); the
    /// Display instance is threaded through via io_config.user_ctx instead.
    static auto onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *userCtx)
        -> bool;

    /**
     * @brief Physical storage row for logical row `y`.
     *
     * The needed X-mirror is set in hardware (esp_lcd_panel_mirror in the constructor); Y is
     * flipped here in software because combining both mirrors in hardware produced a broken
     * image on the S3 unit. Folding the flip into the pixel accessors (instead of an in-place
     * row-swap done once per presentFrame()/waitForFlushDone() pair) means every block always
     * holds pixels in the exact order presentFrame() needs to send them -- no per-frame
     * full-buffer pass, and it composes cleanly with the block-based storage below.
     */
    static inline constexpr int physicalRow(int y)
    {
#if CONFIG_IDF_TARGET_ESP32
        return y; // CYD: no flip verified necessary yet, see CYD-branch.md.
#else
        return kDisplayHeight - 1 - y;
#endif
    }

    /**
     * @brief Storage byte order for one RGB565 pixel -- its own inverse, so the same function
     *        encodes on write and decodes on read.
     *
     * packColor565() always produces standard-layout RGB565 (R at bits[15:11]); every color
     * constant and blend/fade helper in this class assumes that layout. But esp_lcd_ili9341
     * (CYD) sends buffer bytes as-is over SPI and the ILI9341 expects big-endian/high-byte-
     * first per pixel, while Xtensa stores our little-endian uint16_t values low-byte-first --
     * so on that target, pixels are stored byte-swapped, and unswapped back on read, so
     * presentFrame() can send each block straight to the panel with no per-pixel transform or
     * scratch buffer at all.
     */
    static inline constexpr uint16_t storageColor(uint16_t color565)
    {
#if CONFIG_IDF_TARGET_ESP32
        return __builtin_bswap16(color565);
#else
        return color565;
#endif
    }

private:
    esp_lcd_panel_handle_t panel;

    /**
     * Frame buffer storage, split across `blockCount` independently-allocated DMA-capable
     * blocks instead of one heap_caps_malloc() covering the whole kDisplayWidth*kDisplayHeight
     * buffer. See Display()'s allocation loop: internal SRAM on the CYD is fragmented into
     * several non-contiguous heap regions (CYD-branch.md), so a single large request can fail
     * even when the SUM of free regions would be enough -- allocating N smaller blocks, each
     * sized to fit whatever the largest free region was at the time, uses that whole sum
     * instead of being capped by the single largest region. On the S3 (ample contiguous PSRAM
     * is not used for this buffer, but internal SRAM is not fragmented either) this almost
     * always resolves to a single block, same as the old plain allocation.
     *
     * Every block except possibly the last holds exactly `rowsPerBlock` whole rows
     * (kDisplayWidth pixels each); the last block holds the remainder. writePx()/readPx() pick
     * a block via `physicalRow(y) / rowsPerBlock`. kMaxBlocks is a worst-case bound (1 row per
     * block); actual usage is `blockCount`, almost always far smaller.
     */
    static constexpr int kMaxBlocks = kDisplayHeight;
    uint16_t *blocks[kMaxBlocks] = {};
    int blockCount = 0;
    int rowsPerBlock = 0;

    static constexpr auto kDisplayTag = "display";

    /// Given once by onColorTransDone() (an ISR callback) per completed DMA transaction; drained
    /// by waitForFlushDone(), which takes it `pendingFlushCount` times. Counting, not binary --
    /// presentFrame() queues every block without waiting (see its docstring), so up to
    /// `blockCount` completions can be outstanding at once, not just one.
    SemaphoreHandle_t s_flushDone = nullptr;

    /// Number of not-yet-drained completions left over from the most recent presentFrame()
    /// call; set to that frame's blockCount when it queues, decremented once per
    /// waitForFlushDone() take. Single-producer/single-consumer (one presentFrame() is always
    /// followed by one waitForFlushDone() before the next presentFrame(), see both docstrings),
    /// so plain int is enough -- no atomic/lock needed.
    int pendingFlushCount = 0;
};
