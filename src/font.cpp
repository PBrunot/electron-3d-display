// Font rendering logic -- hand-maintained, NOT generated. The glyph data this draws
// (kFontSmall/kFontLarge/kFontHuge's width/row tables) lives in font_data.h, produced by
// tools/font_gen/generate_font.py (see that file and tools/font_gen/README.md to change
// the typeface or add/resize a baked font). Splitting data from logic this way means
// regenerating font_data.h can never silently delete a hand-added function here, unlike
// the previous single monolithic generated font.cpp.
//
// drawChar()/drawCharScaled() are internal (file-local) implementation details: nothing
// outside this file draws a single glyph directly, every caller goes through
// drawText()/drawTextScaled(). The *Impl() templates below are shared across all three
// baked sizes' distinct row-storage types (FontBase<uint8_t/uint16_t/uint64_t>, see
// font.h) so the pixel-plotting logic itself is written once; the public drawText() etc.
// overloads in font.h are just thin per-type wrappers over them.
#include "font.h"

#include "display.h"
#include "font_data.h"

namespace
{
    template <typename RowT>
    void drawCharImpl(uint16_t *frameBuf, int x, int y, char c, uint16_t color, const FontBase<RowT> &font)
    {
        int index = int(uint8_t(c)) - int(uint8_t(font.firstChar));
        if (index < 0 || index >= font.glyphCount)
            return;
        uint8_t width = font.glyphWidths[index];
        const RowT *rows = font.glyphRows + size_t(index) * font.height;
        for (int row = 0; row < font.height; row++)
        {
            int py = y + row;
            if (py < 0 || py >= Display::kDisplayHeight)
                continue;
            RowT bits = rows[row];
            for (int col = 0; col < width; col++)
            {
                if (!(bits & (RowT(1) << (width - 1 - col))))
                    continue;
                int px = x + col;
                if (px < 0 || px >= Display::kDisplayWidth)
                    continue;
                frameBuf[py * Display::kDisplayWidth + px] = color;
            }
        }
    }

    template <typename RowT>
    uint8_t glyphAdvanceImpl(const FontBase<RowT> &font, char c)
    {
        int index = int(uint8_t(c)) - int(uint8_t(font.firstChar));
        uint8_t w = (index >= 0 && index < font.glyphCount) ? font.glyphWidths[index] : 0;
        return w + font.spacing;
    }

    template <typename RowT>
    int drawTextImpl(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const FontBase<RowT> &font)
    {
        int cursorX = x;
        for (const char *p = text; *p; p++)
        {
            drawCharImpl(frameBuf, cursorX, y, *p, color, font);
            cursorX += glyphAdvanceImpl(font, *p);
        }
        return cursorX;
    }

    template <typename RowT>
    int textWidthImpl(const char *text, const FontBase<RowT> &font)
    {
        int total = 0;
        for (const char *p = text; *p; p++)
            total += glyphAdvanceImpl(font, *p);
        return total;
    }

    template <typename RowT>
    void drawCharScaledImpl(uint16_t *frameBuf, int x, int y, char c, uint16_t color, const FontBase<RowT> &font,
                             int scale)
    {
        if (scale <= 1)
        {
            drawCharImpl(frameBuf, x, y, c, color, font);
            return;
        }
        int index = int(uint8_t(c)) - int(uint8_t(font.firstChar));
        if (index < 0 || index >= font.glyphCount)
            return;
        uint8_t width = font.glyphWidths[index];
        const RowT *rows = font.glyphRows + size_t(index) * font.height;
        for (int row = 0; row < font.height; row++)
        {
            RowT bits = rows[row];
            int by = y + row * scale;
            if (by + scale <= 0 || by >= Display::kDisplayHeight)
                continue;
            for (int col = 0; col < width; col++)
            {
                if (!(bits & (RowT(1) << (width - 1 - col))))
                    continue;
                int bx = x + col * scale;
                if (bx + scale <= 0 || bx >= Display::kDisplayWidth)
                    continue;
                for (int sy = 0; sy < scale; sy++)
                {
                    int py = by + sy;
                    if (py < 0 || py >= Display::kDisplayHeight)
                        continue;
                    for (int sx = 0; sx < scale; sx++)
                    {
                        int px = bx + sx;
                        if (px < 0 || px >= Display::kDisplayWidth)
                            continue;
                        frameBuf[py * Display::kDisplayWidth + px] = color;
                    }
                }
            }
        }
    }

    template <typename RowT>
    int drawTextScaledImpl(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color,
                            const FontBase<RowT> &font, int scale)
    {
        if (scale <= 1)
            return drawTextImpl(frameBuf, x, y, text, color, font);
        int cursorX = x;
        for (const char *p = text; *p; p++)
        {
            drawCharScaledImpl(frameBuf, cursorX, y, *p, color, font, scale);
            cursorX += glyphAdvanceImpl(font, *p) * scale;
        }
        return cursorX;
    }
} // namespace

int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const FontSmall &font)
{
    return drawTextImpl(frameBuf, x, y, text, color, font);
}

int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const Font &font)
{
    return drawTextImpl(frameBuf, x, y, text, color, font);
}

int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const FontHuge &font)
{
    return drawTextImpl(frameBuf, x, y, text, color, font);
}

int textWidth(const char *text, const FontSmall &font)
{
    return textWidthImpl(text, font);
}

int textWidth(const char *text, const Font &font)
{
    return textWidthImpl(text, font);
}

int textWidth(const char *text, const FontHuge &font)
{
    return textWidthImpl(text, font);
}

int drawTextScaled(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const FontSmall &font,
                    int scale)
{
    return drawTextScaledImpl(frameBuf, x, y, text, color, font, scale);
}

int drawTextScaled(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const Font &font, int scale)
{
    return drawTextScaledImpl(frameBuf, x, y, text, color, font, scale);
}

int drawTextScaled(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const FontHuge &font,
                    int scale)
{
    return drawTextScaledImpl(frameBuf, x, y, text, color, font, scale);
}

int textWidthScaled(const char *text, const FontSmall &font, int scale)
{
    return scale <= 1 ? textWidthImpl(text, font) : textWidthImpl(text, font) * scale;
}

int textWidthScaled(const char *text, const Font &font, int scale)
{
    return scale <= 1 ? textWidthImpl(text, font) : textWidthImpl(text, font) * scale;
}

int textWidthScaled(const char *text, const FontHuge &font, int scale)
{
    return scale <= 1 ? textWidthImpl(text, font) : textWidthImpl(text, font) * scale;
}
