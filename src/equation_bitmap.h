// GENERATED FILE -- do not hand-edit. Regenerate with
// tools/equation_gen/render_equations.py. See that script for what this
// bitmap shows and why it's an image instead of on-device font glyphs.
#pragma once

#include <cstdint>

constexpr int kEquationBitmapWidth = 216;
constexpr int kEquationBitmapHeight = 80;
constexpr int kEquationBitmapRowBytes = 27;
extern const uint8_t kEquationBitmapData[];

/** Blit the equation backdrop, top-left at (x, y), OR'ing `color` into frameBuf
 * wherever a bit is set and leaving every other pixel untouched (so it composites
 * under other text/points already drawn, and under whatever's drawn after it,
 * matching this project's usual "caller clears first" draw-function contract).
 * Bounds-checked like every other direct-pixel draw in this project. */
void drawEquationBackdrop(uint16_t *frameBuf, int x, int y, uint16_t color);
