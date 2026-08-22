// On-device bitmap font, rasterized offline from a real typeface (Jersey10-Regular, see
// tools/font_gen/) instead of hand-authored per-glyph. This header declares the Font
// types and the rendering API; font.cpp implements it by hand. The glyph DATA tables (the
// part tools/font_gen/generate_font.py actually generates) live in font_data.h, included
// only by font.cpp -- regenerating that file (a new font or size) can never touch the
// hand-maintained rendering logic here, since the generator never emits this file or
// font.cpp. Three fixed sizes are baked in (kFontSmall, kFontLarge, kFontHuge); every call
// site picks one explicitly, there is no "current font" global state.
#pragma once

#include <cstdint>

class Display;

/**
 * One rasterized font: a flat glyph table indexed by (char - firstChar), each glyph
 * keeping its own proportional width (the source font's own advance, not a fixed cell).
 * glyphRows holds `height` row-bitmaps per glyph, row-major (glyphRows + index * height);
 * bit (width-1-col) of a row set = that column lit, row 0 = top. Characters outside
 * [firstChar, firstChar + glyphCount) draw as nothing (same "unmapped = blank" rule the
 * original hand font used) and advance by `spacing` alone.
 *
 * RowT is the glyph row's bit-storage width, picked per baked size to fit its own widest
 * glyph and no wider: kFontSmall's widest glyph is under 8px (RowT = uint8_t), kFontLarge's
 * under 16px (RowT = uint16_t), kFontHuge's up to ~35px (RowT = uint64_t, see
 * font_data.h). A single wide-enough-for-everyone row type would cost the smaller fonts
 * flash space on row bits they can never set.
 */
template <typename RowT>
struct FontBase
{
    const uint8_t *glyphWidths; // [glyphCount]
    const RowT *glyphRows;      // [glyphCount * height], row-major per glyph
    int glyphCount;
    char firstChar;
    uint8_t height;      // pixel rows per glyph
    uint8_t lineAdvance; // px between successive text lines (for manual line wrapping)
    uint8_t spacing;     // px gap appended after every glyph's own width
};

/** Row storage for kFontSmall -- its widest glyph is under 8px. */
using FontSmall = FontBase<uint16_t>; // quick test: widened from uint8_t for DejaVu's wider glyphs

/**
 * Row storage for kFontLarge -- its widest glyph is under 16px. Kept as the plain "Font"
 * name since it's the size generic call sites (chooser.cpp, ticker.h) are written against.
 */
using Font = FontBase<uint32_t>; // quick test: widened from uint16_t for DejaVu's wider glyphs

/** Row storage for kFontHuge -- its widest glyphs (e.g. '#', '@') run past 32px. */
using FontHuge = FontBase<uint64_t>;

/**
 * Two physics-symbol glyphs baked in past the printable-ASCII range (0x20-0x7E), sourced
 * from DejaVuSans.ttf (see tools/font_gen/generate_font.py) since Jersey10 has none. Each
 * is a one-char C string, so it drops straight into a snprintf format or a drawText() call
 * (e.g. drawText(fb, x, y, kGlyphScriptL, color, kFontLarge)).
 */
constexpr char kGlyphElectron[] = "\x7F"; // electron symbol: e with superscript minus
constexpr char kGlyphScriptL[] = "\x80";  // script small l (U+2113): orbital angular-momentum quantum number l

/** Secondary/readout text: the tiled electron-symbol backdrop behind the dissection intro card. */
extern const FontSmall kFontSmall;

/**
 * Titles (element/orbital name, electron configuration) and other mid-size labels (scale
 * bar legend) -- kFontLarge is the size to reach for whenever text needs to be bigger than
 * kFontSmall but doesn't warrant integer-upscaling (see kFontHuge's doc comment below for
 * why that looks bad on-device).
 */
extern const Font kFontLarge;

/**
 * Hero text: the big element-symbol title in atom_view.cpp, and the big shell-notation
 * label ("2p", etc.) during atom_view.cpp's dissection sequence. Its own true 54px design
 * size, not kFontLarge integer-upscaled via drawTextScaled() -- that looked blocky
 * on-device (nearest-neighbor pixel doubling/tripling instead of real glyph shapes at
 * that size).
 */
extern const FontHuge kFontHuge;

/**
 * Draw a null-terminated string left-to-right starting at (x, y), advancing by each
 * glyph's own proportional width + font.spacing. No wrapping -- callers that need
 * line-wrapping (e.g. the per-shell-colored atom title) handle it themselves using
 * font.lineAdvance, same division of labor as before.
 *
 * @return  The x coordinate just past the last glyph drawn, so a caller can continue a
 *          multi-color line (e.g. atom_view's shell-colored title segments) from there.
 *
 * Only the overloads each baked size is actually drawn with unscaled are exposed here
 * (all three) -- per-glyph drawChar() stays a font.cpp-internal implementation detail,
 * nothing outside it calls a single glyph directly.
 */
int drawText(Display &display, int x, int y, const char *text, uint16_t color, const FontSmall &font);
int drawText(Display &display, int x, int y, const char *text, uint16_t color, const Font &font);
int drawText(Display &display, int x, int y, const char *text, uint16_t color, const FontHuge &font);

/** Pixel width of `text` if drawn with drawText() in `font` -- for right-aligned/centered layout. */
int textWidth(const char *text, const FontSmall &font);
int textWidth(const char *text, const Font &font);
int textWidth(const char *text, const FontHuge &font);

/**
 * Like drawText(), but each font pixel becomes a `scale` x `scale` block -- this is how a
 * caller gets a bigger look out of kFontSmall/kFontLarge/kFontHuge (e.g. atom_view.cpp's
 * dissection shell label, ticker.h's scrolling banners, overlay.cpp's scale-bar label)
 * without a dedicated baked size. scale <= 1 behaves exactly like drawText().
 */
int drawTextScaled(Display &display, int x, int y, const char *text, uint16_t color, const FontSmall &font,
                    int scale);
int drawTextScaled(Display &display, int x, int y, const char *text, uint16_t color, const Font &font, int scale);
int drawTextScaled(Display &display, int x, int y, const char *text, uint16_t color, const FontHuge &font,
                    int scale);

/** Like textWidth(), scaled -- see drawTextScaled(). */
int textWidthScaled(const char *text, const FontSmall &font, int scale);
int textWidthScaled(const char *text, const Font &font, int scale);
int textWidthScaled(const char *text, const FontHuge &font, int scale);

/**
 * Draws `text` horizontally centered on the display at row `y` (x picked from
 * textWidthScaled() so it's centered at whatever scale is drawn at -- see drawTextScaled(),
 * scale <= 1 behaves like plain drawText()).
 */
void drawTextCentered(Display &display, int y, const char *text, uint16_t color, const FontSmall &font,
                       int scale = 1);
void drawTextCentered(Display &display, int y, const char *text, uint16_t color, const Font &font, int scale = 1);
void drawTextCentered(Display &display, int y, const char *text, uint16_t color, const FontHuge &font,
                       int scale = 1);
