// Full-screen solid-color calibration test, ESP-IDF/esp_lcd path (unlike
// examples/corner_calibration's Arduino/TFT_eSPI sketch, which calibrated corner
// POSITION, not color, on the older framework). Originally written to chase what looked
// like a per-channel color-packing bug in display.h's packColor565() (compound colors
// like the orbital viewer's orange/yellow reading green-dominant) -- the real cause
// turned out to be display.cpp's esp_lcd_panel_dev_config_t missing `data_endian` and
// using the wrong `rgb_ele_order` (fixed 2026-08-18, see CLAUDE.md's "scambio G/B"
// section for the full misdiagnosis-then-correction history). Kept as a standing
// diagnostic in case panel color issues resurface. See main.cpp's COLOR_TEST toggle.
#pragma once

#include "display.h"

/** Cycle forever through labeled full-screen color swatches (see color_calibration_test.cpp
 * for the exact list), each held a few seconds and logged via ESP_LOGI (RGB intent + raw565
 * hex) -- report which physical color each swatch actually shows to diagnose this panel's
 * color-channel mapping. */
void runColorCalibrationTest(Display &display);
