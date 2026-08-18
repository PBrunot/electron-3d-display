/**
 * @file display.h
 * @brief ESP-IDF `esp_lcd` SPI bring-up and framebuffer management for the ST7789V2 240x240
 *        panel on the Waveshare ESP32-S3-LCD-1.3.
 *
 * All pixel colors in this project MUST be produced via packColor565() (or the palette
 * constants below, which are already packed). The panel's `esp_lcd_panel_dev_config_t` is
 * configured (see display.cpp) so that plain textbook RGB565 packing is correct on this
 * hardware -- no channel-swap or byte-swap compensation is needed in software.
 *
 * Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/spi_lcd.html
 */
#pragma once

#include <cstdint>

#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
    Display(esp_lcd_panel_handle_t panel, uint16_t *frameBuf);

    /**
     * @brief Bring up backlight, SPI bus, ST7789 panel, and a DMA-capable frame buffer.
     * @note Aborts via ESP_ERROR_CHECK on any failure.
     */
    Display();
    ~Display();

    Display(const Display &) = delete;
    Display(Display &&) = delete;
    Display &operator=(const Display &) = delete;
    Display &operator=(Display &&) = delete;

    /**
     * @brief Queue the whole frame buffer for transfer to the panel; returns immediately.
     *
     * `esp_lcd_panel_draw_bitmap()` is asynchronous: it queues DMA transactions and returns
     * as soon as the last chunk is submitted, not once it's transmitted. Callers MUST call
     * waitForFlushDone() before writing into the frame buffer again, or DMA may still be
     * reading the tail of this frame while it's being overwritten. Kept separate from a
     * blocking present so a caller can do other work between queuing and needing the buffer
     * back.
     */
    void presentFrame();

    auto getFrameBuf() -> uint16_t *;

    /**
     * @brief Block until the most recently queued presentFrame() transfer has fully finished.
     *
     * Synchronized via the IO layer's on_color_trans_done callback, which fires exactly once
     * per presentFrame() call on its true last DMA chunk. Call this before overwriting the
     * frame buffer for a new frame.
     */
    auto waitForFlushDone() -> bool;

    /**
     * @brief Pack an (r, g, b) triple (each 0-255) into RGB565 (R5-G6-B5).
     *
     * Standard bit layout, no per-project compensation -- the panel's esp_lcd config
     * (rgb_ele_order / data_endian, set in the constructor) is what makes this correct on
     * this hardware. Every color in this project must be packed through this function rather
     * than built by hand.
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
     * it, producing motion trails. See camera.h's fadeFrameBuffer() / kPersistenceKeepQ8.
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
    static constexpr int kDisplayWidth = 240;
    static constexpr int kDisplayHeight = 240;

    /// Plain function pointer required by on_color_trans_done (no implicit `this`); the
    /// Display instance is threaded through via io_config.user_ctx instead.
    static auto onColorTransDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *userCtx)
        -> bool;

private:
    esp_lcd_panel_handle_t panel;
    uint16_t *frameBuf; ///< DMA-capable, kDisplayWidth*kDisplayHeight RGB565 pixels.

    /**
     * Row-flipped copy of frameBuf, used as the DMA source for presentFrame(). The needed
     * X-mirror is set in hardware (esp_lcd_panel_mirror in the constructor); Y is flipped
     * here in software because combining both mirrors in hardware produced a broken image on
     * this unit. Kept separate from frameBuf (rather than flipping in place) because callers
     * persist frameBuf across frames for fading/trails (see camera.h's fadeFrameBuffer()) --
     * an in-place flip would double-flip on the next frame's draw calls, which address
     * frameBuf in normal row order. Lazily allocated on first presentFrame() so the two-arg
     * test constructor, which has no DMA-capable frameBuf either, doesn't need it.
     */
    uint16_t *presentBuf = nullptr;

    static constexpr auto kDisplayTag = "display";

    /// Given by onColorTransDone() (an ISR callback) and taken by waitForFlushDone().
    SemaphoreHandle_t s_flushDone = nullptr;
};
