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
     * Pack a physical-intent (r, g, b) (each 0-255) into this specific panel's raw RGB565
     * value. This panel's G and B subpixel drive lines are physically swapped (verified
     * empirically: sending standard-RGB565 GREEN displays as blue and vice versa, RED
     * unaffected -- see CLAUDE.md's "scambio G/B" section). No ST7789 register fixes a G/B
     * swap, so every color anywhere in this project must be packed through this function --
     * never build an RGB565 value by hand elsewhere. Verified against CLAUDE.md's four
     * full-screen test constants (red/green/blue/yellow all correct with this formula).
     */
    static inline constexpr uint16_t packColor565(uint8_t r, uint8_t g, uint8_t b)
    {
        return uint16_t(((r >> 3) << 11) | ((b >> 2) << 5) | (g >> 3));
    }

    /** Inverse of packColor565() -- recovers the physical-intent (r, g, b) packColor565()
     * was called with, each expanded back to 8 bits (5/6-bit field replicated into the low
     * bits, the usual RGB565-to-24-bit expansion). Needed by blendColor565()/fadeColor565()
     * below, which must read a pixel already in the frame buffer before combining it with a
     * new color -- see CLAUDE.md's "scambio G/B" section for why the bit layout isn't the
     * textbook RGB565 one.
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
    static constexpr int kDisplayWidth = 240;
    static constexpr int kDisplayHeight = 240;

    // Static: on_color_trans_done needs a plain function pointer (no implicit `this`),
    // so the Display instance is threaded through instead via io_config.user_ctx.
    static auto onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *userCtx)
        -> bool;

private:
    esp_lcd_panel_handle_t panel;
    uint16_t *frameBuf; // DMA-capable, Display::kDisplayWidth*kDisplayHeight RGB565 pixels
    static constexpr auto kDisplayTag = "display";

    // Given by onColorTransDone() (an ISR callback -- see esp_lcd_panel_io_spi_config_t's
    // on_color_trans_done, fired exactly once per presentFrame() call, on its true last DMA
    // chunk) and taken by waitForFlushDone() -- see that method's doc comment above for why
    // callers must call it before reusing frameBuf.
    SemaphoreHandle_t s_flushDone = nullptr;
};
