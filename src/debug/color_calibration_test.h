/**
 * @file color_calibration_test.h
 * @brief Full-screen solid-color calibration test, ESP-IDF/esp_lcd path.
 *
 * Standing diagnostic for panel color-channel mapping issues: cycles through labeled
 * full-screen swatches, logging each one's intended RGB and packed RGB565 value, so a viewer
 * can compare what's logged against what the panel physically shows. Kept around in case a
 * future panel/lot exhibits a color-channel mapping bug (unlike display.h's
 * packColor565()/the esp_lcd panel config, which are unit-verified as of this writing --
 * see main.cpp's COLOR_TEST toggle to run this).
 */
#pragma once

#include "render/display.h"

/// Cycle forever through labeled full-screen color swatches (see color_calibration_test.cpp
/// for the exact list), each held a few seconds and logged via ESP_LOGI (RGB intent + raw565
/// hex) -- report which physical color each swatch actually shows to diagnose this panel's
/// color-channel mapping.
void runColorCalibrationTest(Display &display);
