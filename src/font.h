// Minimal on-device bitmap font: 5x7 glyphs (5 columns, 7 rows), 1px spacing between
// characters/lines (6px advance / 8px line height). No font asset exists anywhere in this
// repo to port from -- the MicroPython overlays draw text via
// framebuf.FrameBuffer.text(), whose glyph bitmap lives inside the MicroPython firmware
// itself, not in this repo's source (see font.cpp's header comment for the full story and
// the authoring methodology used instead).
#pragma once

#include <cstdint>

constexpr int kFontGlyphWidth = 5;
constexpr int kFontGlyphHeight = 7;
constexpr int kFontAdvanceX = 6; // glyph width + 1px spacing
constexpr int kFontAdvanceY = 8; // glyph height + 1px spacing

/**
 * Blit one glyph at (x, y) top-left, in `color`, into a Display::kDisplayWidth*kDisplayHeight
 * RGB565 buffer. Per-pixel bounds-checked, safe to call with a glyph partially or fully
 * off-screen. Background is left untouched (no fill) -- caller clears the frame first,
 * matching every other draw function in this project. Unmapped characters (see
 * font.cpp's glyphFor()) draw as blank rather than erroring.
 */
void drawChar(uint16_t *frameBuf, int x, int y, char c, uint16_t color);

/**
 * Draw a null-terminated string left-to-right starting at (x, y), each glyph
 * kFontAdvanceX apart. No wrapping -- callers that need line-wrapping (e.g. the
 * per-shell-colored atom title, M4) handle it themselves, same division of labor as
 * micropython/atom_view.py's _draw_atom_title() vs framebuf.text().
 *
 * @return  The x coordinate just past the last glyph drawn, so a caller can continue a
 *          multi-color line (e.g. atom_view's shell-colored title segments) from there.
 */
int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color);

/** Pixel width of `text` if drawn with drawText() -- for right-aligned/centered layout. */
int textWidth(const char *text);
