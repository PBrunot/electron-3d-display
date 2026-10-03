// Single source of truth for which board this firmware is being built for. Prefer this over
// testing CONFIG_IDF_TARGET_ESP32/BOARD_ES3C28P directly: kBoard is a normal enum, so callers
// branch on it with `if constexpr` (or a ternary for a constant's value) instead of the
// preprocessor.
//
// A real per-target *compilation* difference -- a header or component only available for one
// target (see render/display.cpp's esp_lcd_ili9341.h/src/idf_component.yml, gated to
// `target in [esp32, esp32s3]`), or a static array/constant whose value carries its own long
// per-target tuning history as an inline comment (config/visual_constants.h's
// kOrbitalNumPoints/kAtomNumPoints, which stay keyed off the raw CONFIG_IDF_TARGET_ESP32 macro
// since they're about PSRAM availability, a chip-family trait both S3 boards share) -- still
// needs a real #if/#else/#endif; kBoard only replaces the *runtime-logic* branches that used to
// ride along with those, like main.cpp's IMU-vs-touch setup or display.h's byte-order/row-flip
// helpers.
#pragma once

#include "sdkconfig.h" // CONFIG_IDF_TARGET_ESP32

enum class Board
{
    kWaveshareS3, // Waveshare ESP32-S3-LCD-1.3: ESP32-S3, PSRAM, ST7789V2 240x240 panel, QMI8658 IMU.
    kCYD,         // CYD (ESP32-2432S028R): plain ESP32 (Xtensa LX6), no PSRAM, ILI9341 240x320
                  // panel, XPT2046 resistive touch, no IMU.
    kES3C28P,     // QDtech ES3C28P (boards/cyd-esp32s3/): ESP32-S3, PSRAM, ILI9341V 240x320
                  // panel, FT6336G capacitive touch, no IMU.
};

// CONFIG_IDF_TARGET_ESP32 is left undefined (not defined-to-0) by either S3 build, so it can't
// appear as a bare identifier in an `if constexpr`/ternary condition -- this #ifdef is the one
// place in the codebase that reads the raw macro. BOARD_ES3C28P is this project's own macro
// (platformio.ini's `[env:ES3C28P]` build_flags), needed because the ES3C28P and Waveshare S3
// boards share the same esp32s3 IDF target and so can't be told apart by CONFIG_IDF_TARGET_*
// alone.
#if defined(CONFIG_IDF_TARGET_ESP32)
inline constexpr Board kBoard = Board::kCYD;
#elif defined(BOARD_ES3C28P)
inline constexpr Board kBoard = Board::kES3C28P;
#else
inline constexpr Board kBoard = Board::kWaveshareS3;
#endif

/// True only for the CYD -- kept as its own constant (rather than always spelling out
/// `kBoard == Board::kCYD`) since it's still the most common branch (chip family: plain ESP32
/// vs either S3 board).
inline constexpr bool kIsCYD = kBoard == Board::kCYD;

/// True for boards with a 240x320 ILI9341-family panel driven via esp_lcd_new_panel_ili9341()
/// (the CYD's clone chip, ES3C28P's ILI9341V) instead of the Waveshare S3's 240x240 ST7789V2 --
/// drives display.h's kDisplayHeight and its byte-order/row-flip quirks, which are traits of
/// that shared driver/panel family, not of which chip is running it.
inline constexpr bool kHasIli9341Panel = kBoard == Board::kCYD || kBoard == Board::kES3C28P;

/// True for boards navigated via a touch panel instead of IMU tilt -- both the CYD and ES3C28P
/// have no IMU wired up (see ux/imu.cpp's no-op constructor). Different touch hardware/protocol
/// per board (ux/touch.h's bit-banged XPT2046 vs ux/ft6336g.h's I2C FT6336G) still needs its own
/// branch (see main.cpp), this only replaces the outer "touch vs tilt" decision.
inline constexpr bool kHasTouchNav = kBoard == Board::kCYD || kBoard == Board::kES3C28P;
