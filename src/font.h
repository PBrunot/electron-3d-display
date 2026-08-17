// On-device bitmap font, rasterized offline from a real typeface (Jersey10-Regular, see
// tools/font_gen/) instead of hand-authored per-glyph -- see font.cpp's header comment
// for the source/license and regeneration instructions. Two fixed sizes are baked in
// (kFontSmall, kFontLarge); every call site picks one explicitly, there is no "current
// font" global state.
#pragma once

#include <cstdint>

/**
 * One rasterized font: a flat glyph table indexed by (char - firstChar), each glyph
 * keeping its own proportional width (the source font's own advance, not a fixed cell).
 * glyphRows holds `height` row-bitmaps per glyph, row-major (glyphRows + index * height);
 * bit (width-1-col) of a row set = that column lit, row 0 = top. Characters outside
 * [firstChar, firstChar + glyphCount) draw as nothing (same "unmapped = blank" rule the
 * original hand font used) and advance by `spacing` alone.
 */
struct Font
{
    const uint8_t *glyphWidths; // [glyphCount]
    const uint16_t *glyphRows;  // [glyphCount * height], row-major per glyph
    int glyphCount;
    char firstChar;
    uint8_t height;      // pixel rows per glyph
    uint8_t lineAdvance; // px between successive text lines (for manual line wrapping)
    uint8_t spacing;     // px gap appended after every glyph's own width
};

/** Secondary/readout text: FPS counter, scale bar label. */
extern const Font kFontSmall;

/** Titles (element/orbital name, electron configuration). */
extern const Font kFontLarge;

/**
 * Blit one glyph at (x, y) top-left, in `color`, into a Display::kDisplayWidth*kDisplayHeight
 * RGB565 buffer. Per-pixel bounds-checked, safe to call with a glyph partially or fully
 * off-screen. Background is left untouched (no fill) -- caller clears the frame first,
 * matching every other draw function in this project.
 */
void drawChar(uint16_t *frameBuf, int x, int y, char c, uint16_t color, const Font &font);

/**
 * Draw a null-terminated string left-to-right starting at (x, y), advancing by each
 * glyph's own proportional width + font.spacing. No wrapping -- callers that need
 * line-wrapping (e.g. the per-shell-colored atom title) handle it themselves using
 * font.lineAdvance, same division of labor as before.
 *
 * @return  The x coordinate just past the last glyph drawn, so a caller can continue a
 *          multi-color line (e.g. atom_view's shell-colored title segments) from there.
 */
int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const Font &font);

/** Pixel width of `text` if drawn with drawText() in `font` -- for right-aligned/centered layout. */
int textWidth(const char *text, const Font &font);
