// GENERATED FILE -- do not hand-edit. Regenerate with
// tools/splash_gen/render_splash.py. See that script for why this is a
// raw pre-packed pixel array instead of an on-device JPEG decode.
#pragma once

#include <cstdint>

class Display;

inline constexpr int kSplashBitmapWidth = 240;
inline constexpr int kSplashBitmapHeight = 240;
// Already packed via Display::packColor565()'s exact bit formula (plain
// textbook RGB565) -- draw with a plain copy, never re-pack these values.
extern const uint16_t kSplashBitmapData[];

/** Blit the splash image at (0, 0), opaque (no blending) -- caller presents the
 * frame afterward. Clipped against Display::kDisplayWidth/Height like every
 * other draw function in this project (see Display::blit()); on a panel taller
 * than kSplashBitmapHeight (e.g. the CYD's 320px) the rest of the screen is left
 * whatever it already held -- caller clears first if that matters. */
void drawSplashScreen(Display &display);
