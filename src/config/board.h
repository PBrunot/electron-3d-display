// Single source of truth for which board this firmware is being built for. Prefer this over
// testing CONFIG_IDF_TARGET_ESP32 directly: kIsCYD is a normal bool, so callers branch on it
// with `if constexpr` (or a ternary for a constant's value) instead of the preprocessor.
//
// A real per-target *compilation* difference -- a header or component only available for one
// target (see render/display.cpp's esp_lcd_ili9341.h/src/idf_component.yml, gated to
// `target == esp32`), or a static array/constant whose value carries its own long per-target
// tuning history as an inline comment (config/visual_constants.h's kOrbitalNumPoints/
// kAtomNumPoints) -- still needs a real #if/#else/#endif; kIsCYD only replaces the
// *runtime-logic* branches that used to ride along with those, like main.cpp's IMU-vs-touch
// setup or display.h's byte-order/row-flip helpers.
#pragma once

#include "sdkconfig.h" // CONFIG_IDF_TARGET_ESP32

// true: CYD (ESP32-2432S028R, "Cheap Yellow Display") -- plain ESP32 (Xtensa LX6), no PSRAM,
//       ILI9341 240x320 panel, XPT2046 resistive touch, no IMU.
// false: Waveshare ESP32-S3-LCD-1.3 -- ESP32-S3, PSRAM, ST7789V2 240x240 panel, QMI8658 IMU.
//
// CONFIG_IDF_TARGET_ESP32 is left undefined (not defined-to-0) by the S3 build, so it can't
// appear as a bare identifier in an `if constexpr`/ternary condition -- this #ifdef is the one
// place in the codebase that reads the raw macro.
#ifdef CONFIG_IDF_TARGET_ESP32
inline constexpr bool kIsCYD = true;
#else
inline constexpr bool kIsCYD = false;
#endif
