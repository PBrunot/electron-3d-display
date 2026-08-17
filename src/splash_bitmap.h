// GENERATED FILE -- do not hand-edit. Regenerate with
// tools/splash_gen/render_splash.py. See that script for why this is a
// raw pre-packed pixel array instead of an on-device JPEG decode.
#pragma once

#include <cstdint>

constexpr int kSplashBitmapWidth = 240;
constexpr int kSplashBitmapHeight = 240;
// Already packed via Display::packColor565()'s exact bit formula (this panel's
// G/B-swapped RGB565) -- draw with a plain copy, never re-pack these values.
extern const uint16_t kSplashBitmapData[];

/** Blit the splash image at (0, 0), opaque (no blending) -- caller presents the
 * frame afterward. Bounds-checked against Display::kDisplayWidth/Height like
 * every other draw function in this project, though the emitted size always
 * matches the display exactly (see WIDTH_PX/HEIGHT_PX in the generator). */
void drawSplashScreen(uint16_t *frameBuf);
