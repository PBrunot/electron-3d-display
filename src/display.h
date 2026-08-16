// ESP-IDF esp_lcd SPI display bring-up for the Waveshare ESP32-S3-LCD-1.3 (ST7789V2,
// 240x240). Pinout and the BGR color-order correction per CLAUDE.md sections 2/3.
// Reference: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/spi_lcd.html
//
// Extracted unchanged from main.cpp's original app_main() setup -- see packColor565()
// below for the one hardware quirk every caller must route color through.
#pragma once

#include <cstdint>

#include "esp_lcd_panel_ops.h"

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

struct Display {
    esp_lcd_panel_handle_t panel;
    uint16_t* frameBuf; // DMA-capable, kDisplayWidth*kDisplayHeight RGB565 pixels
};

/**
 * Bring up backlight + SPI bus + ST7789 panel + DMA-capable frame buffer. Aborts via
 * ESP_ERROR_CHECK on failure (matches the rest of this codebase's ESP-IDF error handling).
 * Panel geometry (mirror/rotation) is set here and left as CLAUDE.md's "ancora da
 * verificare" state for this unit's ESP-IDF path -- unchanged from the pre-refactor
 * main.cpp, not re-derived by this milestone.
 */
Display initDisplay();

/**
 * Push the whole frame buffer to the panel, and block until the transfer has actually
 * finished (not just been queued) before returning. esp_lcd_panel_draw_bitmap() itself is
 * asynchronous -- it submits DMA transactions via spi_device_queue_trans() and returns as
 * soon as the last chunk is queued, not once it's transmitted (confirmed in
 * esp_lcd_panel_io_spi.c's panel_io_spi_tx_color()); without this wait, the caller could
 * start overwriting `frameBuf` for the next frame while DMA is still reading the tail of
 * the current one out of that same buffer. Synchronized via the IO layer's
 * on_color_trans_done callback (fired exactly once per draw_bitmap() call, on its true
 * last chunk), wired up in initDisplay().
 */
void presentFrame(const Display& d);

/**
 * Pack a physical-intent (r, g, b) (each 0-255) into this specific panel's raw RGB565
 * value. This panel's G and B subpixel drive lines are physically swapped (verified
 * empirically: sending standard-RGB565 GREEN displays as blue and vice versa, RED
 * unaffected -- see CLAUDE.md's "scambio G/B" section). No ST7789 register fixes a G/B
 * swap, so every color anywhere in this project must be packed through this function --
 * never build an RGB565 value by hand elsewhere. Verified against CLAUDE.md's four
 * full-screen test constants (red/green/blue/yellow all correct with this formula).
 */
constexpr uint16_t packColor565(uint8_t r, uint8_t g, uint8_t b) {
    return uint16_t(((r >> 3) << 11) | ((b >> 2) << 5) | (g >> 3));
}

constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorWhite = 0xFFFF;
