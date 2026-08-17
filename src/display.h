// ESP-IDF esp_lcd SPI display bring-up for the Waveshare ESP32-S3-LCD-1.3 (ST7789V2,
// 240x240). Pinout and the BGR color-order correction per CLAUDE.md sections 2/3.
// Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/spi_lcd.html
//
// Extracted unchanged from main.cpp's original app_main() setup -- see packColor565()
// below for the one hardware quirk every caller must route color through.
#pragma once

#include <cstdint>

#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace detail
{
// Free function, not a Display member: a class's own static constexpr member functions
// aren't usable in a constant expression until the class body finishes parsing ("complete-
// class context"), so Display::packColor565() itself can't be called to initialize the
// kColor* palette below it in the same class -- this standalone helper can, since it's
// already complete at the point it's used. Display::packColor565() just delegates here;
// see that function's docstring for the actual formula rationale.
constexpr uint16_t packColor565Impl(uint8_t r, uint8_t g, uint8_t b)
{
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
} // namespace detail

class Display
{
public:
    Display(esp_lcd_panel_handle_t panel, uint16_t *frameBuf);

    /**
     * Bring up backlight + SPI bus + ST7789 panel + DMA-capable frame buffer. Aborts via
     * ESP_ERROR_CHECK on failure (matches the rest of this codebase's ESP-IDF error handling).
     * Panel geometry (mirror/rotation) is set here and left as CLAUDE.md's "ancora da
     * verificare" state for this unit's ESP-IDF path -- unchanged from the pre-refactor
     * main.cpp, not re-derived by this milestone.
     */
    Display();
    ~Display();

    Display(const Display &) = delete;
    Display(Display &&) = delete;
    Display &operator=(const Display &) = delete;
    Display &operator=(Display &&) = delete;

    /**
     * Queue the whole frame buffer for transfer to the panel and return immediately --
     * does NOT wait for the transfer to finish. esp_lcd_panel_draw_bitmap() itself is
     * asynchronous -- it submits DMA transactions via spi_device_queue_trans() and returns
     * as soon as the last chunk is queued, not once it's transmitted (confirmed in
     * esp_lcd_panel_io_spi.c's panel_io_spi_tx_color()). Callers MUST call
     * waitForFlushDone() before writing into `frameBuf` again (i.e. before the next frame's
     * render pass), or DMA may still be reading the tail of this frame out of the same
     * buffer while it's being overwritten. Kept as two separate calls (rather than one
     * blocking presentFrame()) so a caller can do other work between queuing this frame and
     * needing the buffer back for the next one.
     */
    void presentFrame();

    auto getFrameBuf() -> uint16_t *;

    /**
     * Block until the most recently queued presentFrame() transfer has actually finished
     * (not just been queued). Synchronized via the IO layer's on_color_trans_done callback
     * (fired exactly once per presentFrame() call, on its true last DMA chunk), wired up in
     * the constructor. Call this before overwriting `frameBuf` for a new frame.
     */
    auto waitForFlushDone() -> bool;

    /**
     * Pack a physical-intent (r, g, b) (each 0-255) into standard textbook RGB565
     * (R5-G6-B5, no per-project bit-field swap) -- bit-for-bit identical to Arduino_GFX's
     * RGB565(r,g,b) macro, confirmed 2026-08-18. The "G and B lines are physically swapped"
     * theory (CLAUDE.md's old "scambio G/B" section) was a misdiagnosis: the real cause was
     * this panel's esp_lcd_panel_dev_config_t missing an explicit `data_endian` (defaulted
     * away from this panel's actual wire format) plus the wrong `rgb_ele_order` -- fixed at
     * the source in display.cpp's Display() constructor (LCD_RGB_ELEMENT_ORDER_RGB +
     * LCD_RGB_DATA_ENDIAN_LITTLE, 2026-08-18) instead of being papered over here with a
     * hand-rolled bit-swapped packing. The old swapped formula "worked" for pure
     * single-channel test colors (where a channel swap and an endianness fix can
     * coincidentally agree) but never correctly for compound colors like this project's
     * orbital orange/yellow -- see CLAUDE.md for the full history. Every color anywhere in
     * this project should still be packed through this function -- never build an RGB565
     * value by hand elsewhere -- but it no longer needs to be panel-quirk-aware; that's the
     * esp_lcd config's job now.
     */
    static inline constexpr uint16_t packColor565(uint8_t r, uint8_t g, uint8_t b)
    {
        return detail::packColor565Impl(r, g, b);
    }

    /** Inverse of packColor565() -- recovers the physical-intent (r, g, b) packColor565()
     * was called with, each expanded back to 8 bits (5/6-bit field replicated into the low
     * bits, the usual RGB565-to-24-bit expansion). Needed by blendColor565()/fadeColor565()
     * below, which must read a pixel already in the frame buffer before combining it with a
     * new color.
     */
    static void unpackColor565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b);

    /**
     * Alpha-blend `target` into `base`, `alphaQ8`/256 of the way (0 = `base` unchanged, 256 =
     * `target` fully replaces it) -- e.g. CLAUDE.md-independent point rendering wants
     * overlapping points to converge toward full brightness rather than the last-drawn point
     * fully overwriting whatever was there (see camera.h's kElectronAlphaQ8 and
     * pc/viewer_common.py's ELECTRON_ALPHA, which this ports).
     */
    static uint16_t blendColor565(uint16_t base, uint16_t target, uint16_t alphaQ8);

    /**
     * Scale `c`'s channels toward black by `keepQ8`/256 (256 = unchanged, 0 = pure black) --
     * used to fade the whole frame buffer toward black instead of hard-clearing it (see
     * camera.h's fadeFrameBuffer()/kPersistenceKeepQ8 and pc/viewer_common.py's
     * PERSISTENCE_DECAY, which this ports).
     */
    static uint16_t fadeColor565(uint16_t c, uint16_t keepQ8);

    static constexpr uint16_t kColorBlack = 0x0000;
    static constexpr uint16_t kColorWhite = 0xFFFF;

    // Named palette, RGB values matching Arduino_GFX's RGB565_* macro set (a widely-used
    // reference palette -- its RGB565(r,g,b) macro is bit-for-bit identical to
    // packColor565() above, confirmed 2026-08-18), computed through detail::packColor565Impl()
    // (not the packColor565() member above -- see its forward-declaration comment for why)
    // rather than copied as raw hex so they always match this panel's actual packing.
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

    static constexpr int kDisplayWidth = 240;
    static constexpr int kDisplayHeight = 240;

    // Static: on_color_trans_done needs a plain function pointer (no implicit `this`),
    // so the Display instance is threaded through instead via io_config.user_ctx.
    static auto onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *userCtx)
        -> bool;

private:
    esp_lcd_panel_handle_t panel;
    uint16_t *frameBuf; // DMA-capable, Display::kDisplayWidth*kDisplayHeight RGB565 pixels

    // Row-flipped copy of frameBuf, DMA source for presentFrame(). Panel Y-mirror was tried
    // via esp_lcd_panel_mirror(panel, true, true) but combining it with the needed X-mirror
    // produced a broken image on this unit (see CLAUDE.md's mirror notes) -- only X-mirror is
    // set in hardware (see the constructor's esp_lcd_panel_mirror call), and Y is flipped here
    // in software instead. Kept separate from frameBuf (rather than flipping frameBuf in
    // place) because callers persist frameBuf across frames for fading/trails (see
    // camera.h's fadeFrameBuffer()) -- flipping it in place would double-flip on the next
    // frame's draw calls, which all address frameBuf in normal (non-flipped) row order.
    // Lazily allocated on first presentFrame() so the test-seam two-arg constructor (which
    // doesn't set up a DMA-capable frameBuf either) doesn't need to know about it.
    uint16_t *presentBuf = nullptr;

    static constexpr auto kDisplayTag = "display";

    // Given by onColorTransDone() (an ISR callback -- see esp_lcd_panel_io_spi_config_t's
    // on_color_trans_done, fired exactly once per presentFrame() call, on its true last DMA
    // chunk) and taken by waitForFlushDone() -- see that method's doc comment above for why
    // callers must call it before reusing frameBuf.
    SemaphoreHandle_t s_flushDone = nullptr;
};
