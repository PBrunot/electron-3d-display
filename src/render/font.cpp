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
#include "render/font.h"

#include "render/display.h"
#include "render/font_data.h"

namespace
{
    /**
     * Maps a baked font's row type to the next-size-down font used to draw a kScriptSub/
     * kScriptSuper span within it (see font.h's doc comment on those). The primary template
     * is the "no smaller font available" fallback (span text just stays at `font`'s own
     * size) -- correct for kFontSmall (RowT = uint16_t), this font system's smallest baked
     * size. kFontLarge (uint32_t) and kFontHuge (uint64_t) each specialize to name their
     * real companion.
     */
    template <typename RowT>
    struct ScriptFont
    {
        static const FontBase<RowT> &get(const FontBase<RowT> &font) { return font; }
    };

    template <>
    struct ScriptFont<uint32_t> // kFontLarge's span font
    {
        static const FontBase<uint16_t> &get(const FontBase<uint32_t> &) { return kFontSmall; }
    };

    template <>
    struct ScriptFont<uint64_t> // kFontHuge's span font
    {
        static const FontBase<uint32_t> &get(const FontBase<uint64_t> &) { return kFontLarge; }
    };

    enum class ScriptMode
    {
        kNormal,
        kSub,
        kSuper
    };

    /**
     * If `c` is one of font.h's three script-markup control bytes, applies it to `*mode` and
     * returns true (caller must draw/advance nothing for it -- these are control bytes, not
     * glyphs, see kScriptSub's doc comment). Returns false for any ordinary character,
     * `*mode` untouched.
     */
    inline bool applyScriptMarker(char c, ScriptMode *mode)
    {
        if (c == kScriptSub[0])
            *mode = ScriptMode::kSub;
        else if (c == kScriptSuper[0])
            *mode = ScriptMode::kSuper;
        else if (c == kScriptEnd[0])
            *mode = ScriptMode::kNormal;
        else
            return false;
        return true;
    }

    template <typename RowT>
    void drawCharImpl(Display &display, int x, int y, char c, uint16_t color, const FontBase<RowT> &font)
    {
        int index = int(uint8_t(c)) - int(uint8_t(font.firstChar));
        if (index < 0 || index >= font.glyphCount)
            return;
        uint8_t width = font.glyphWidths[index];
        const RowT *rows = font.glyphRows + size_t(index) * font.height;
        for (int row = 0; row < font.height; row++)
        {
            int py = y + row;
            RowT bits = rows[row];
            for (int col = 0; col < width; col++)
            {
                if (!(bits & (RowT(1) << (width - 1 - col))))
                    continue;
                display.writePx(x + col, py, color);
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
    int drawTextImpl(Display &display, int x, int y, const char *text, uint16_t color, const FontBase<RowT> &font)
    {
        const auto &scriptFont = ScriptFont<RowT>::get(font);
        ScriptMode mode = ScriptMode::kNormal;
        int cursorX = x;
        for (const char *p = text; *p; p++)
        {
            if (applyScriptMarker(*p, &mode))
                continue;
            if (mode == ScriptMode::kNormal)
            {
                drawCharImpl(display, cursorX, y, *p, color, font);
                cursorX += glyphAdvanceImpl(font, *p);
            }
            else
            {
                // Subscript sits low (bottom-aligned to the main line), superscript sits
                // high (top-aligned to it) -- see font.h's kScriptSub doc comment.
                int scriptY = mode == ScriptMode::kSub ? y + (font.height - scriptFont.height) : y;
                drawCharImpl(display, cursorX, scriptY, *p, color, scriptFont);
                cursorX += glyphAdvanceImpl(scriptFont, *p);
            }
        }
        return cursorX;
    }

    template <typename RowT>
    int textWidthImpl(const char *text, const FontBase<RowT> &font)
    {
        const auto &scriptFont = ScriptFont<RowT>::get(font);
        ScriptMode mode = ScriptMode::kNormal;
        int total = 0;
        for (const char *p = text; *p; p++)
        {
            if (applyScriptMarker(*p, &mode))
                continue;
            total += mode == ScriptMode::kNormal ? glyphAdvanceImpl(font, *p) : glyphAdvanceImpl(scriptFont, *p);
        }
        return total;
    }

    template <typename RowT>
    void drawCharScaledImpl(Display &display, int x, int y, char c, uint16_t color, const FontBase<RowT> &font,
                             int scale)
    {
        if (scale <= 1)
        {
            drawCharImpl(display, x, y, c, color, font);
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
            for (int col = 0; col < width; col++)
            {
                if (!(bits & (RowT(1) << (width - 1 - col))))
                    continue;
                int bx = x + col * scale;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        display.writePx(bx + sx, by + sy, color);
            }
        }
    }

    template <typename RowT>
    int drawTextScaledImpl(Display &display, int x, int y, const char *text, uint16_t color,
                            const FontBase<RowT> &font, int scale)
    {
        if (scale <= 1)
            return drawTextImpl(display, x, y, text, color, font);
        const auto &scriptFont = ScriptFont<RowT>::get(font);
        ScriptMode mode = ScriptMode::kNormal;
        int cursorX = x;
        for (const char *p = text; *p; p++)
        {
            if (applyScriptMarker(*p, &mode))
                continue;
            if (mode == ScriptMode::kNormal)
            {
                drawCharScaledImpl(display, cursorX, y, *p, color, font, scale);
                cursorX += glyphAdvanceImpl(font, *p) * scale;
            }
            else
            {
                int scriptY = mode == ScriptMode::kSub ? y + (font.height - scriptFont.height) * scale : y;
                drawCharScaledImpl(display, cursorX, scriptY, *p, color, scriptFont, scale);
                cursorX += glyphAdvanceImpl(scriptFont, *p) * scale;
            }
        }
        return cursorX;
    }

    template <typename RowT>
    void drawTextCenteredImpl(Display &display, int y, const char *text, uint16_t color, const FontBase<RowT> &font,
                               int scale)
    {
        int width = scale <= 1 ? textWidthImpl(text, font) : textWidthImpl(text, font) * scale;
        int x = (Display::kDisplayWidth - width) / 2;
        drawTextScaledImpl(display, x, y, text, color, font, scale);
    }
} // namespace

int drawText(Display &display, int x, int y, const char *text, uint16_t color, const FontSmall &font)
{
    return drawTextImpl(display, x, y, text, color, font);
}

int drawText(Display &display, int x, int y, const char *text, uint16_t color, const Font &font)
{
    return drawTextImpl(display, x, y, text, color, font);
}

int drawText(Display &display, int x, int y, const char *text, uint16_t color, const FontHuge &font)
{
    return drawTextImpl(display, x, y, text, color, font);
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

int drawTextScaled(Display &display, int x, int y, const char *text, uint16_t color, const FontSmall &font,
                    int scale)
{
    return drawTextScaledImpl(display, x, y, text, color, font, scale);
}

int drawTextScaled(Display &display, int x, int y, const char *text, uint16_t color, const Font &font, int scale)
{
    return drawTextScaledImpl(display, x, y, text, color, font, scale);
}

int drawTextScaled(Display &display, int x, int y, const char *text, uint16_t color, const FontHuge &font,
                    int scale)
{
    return drawTextScaledImpl(display, x, y, text, color, font, scale);
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

void drawTextCentered(Display &display, int y, const char *text, uint16_t color, const FontSmall &font, int scale)
{
    drawTextCenteredImpl(display, y, text, color, font, scale);
}

void drawTextCentered(Display &display, int y, const char *text, uint16_t color, const Font &font, int scale)
{
    drawTextCenteredImpl(display, y, text, color, font, scale);
}

void drawTextCentered(Display &display, int y, const char *text, uint16_t color, const FontHuge &font, int scale)
{
    drawTextCenteredImpl(display, y, text, color, font, scale);
}
