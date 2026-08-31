// Decodes img/atomic_cube.jpg (mirrored at data/atomic_cube.jpg) on-device via jpeg_bg.h.
#pragma once

class Display;

inline constexpr int kSplashBitmapWidth = 240;
inline constexpr int kSplashBitmapHeight = 240;

/// Blits data/atomic_cube.jpg centered on the display, opaque -- see jpeg_bg.h.
void drawSplashScreen(Display &display);
