// Standalone slice-only smoke test: boots straight into a fixed sequence of plane-slice
// heatmaps (see views/orbital_slice.h, SLICE.md), no chooser/IMU/tilt input needed -- the
// quick way to eyeball per-preset patterns and tune config/visual_constants.h's slice
// constants on hardware without going through the full chooser -> orbital view -> Right-hold
// flow. See main.cpp's SLICE_TEST toggle.
#pragma once

#include "render/display.h"

void runOrbitalSliceTest(Display &display);
