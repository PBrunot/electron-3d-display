#!/usr/bin/env python3
"""Rasterize Jersey10-Regular.ttf into src/font.cpp's constexpr glyph tables.

Two sizes are baked in: kFontSmall (secondary/readout text -- FPS counter, scale bar
label) and kFontLarge (titles). Each glyph keeps its own proportional width (the font's
own advance width, not a fixed cell) -- this is what makes the result look like a real
font instead of the old fixed 5x7 grid. Row bits are still emitted as '#'/'.' ASCII art
literals (fed through the rowBits() helper already used in the previous hand-authored
font.cpp) even though this file is now machine-generated, so every glyph's shape stays
directly checkable by eye in font.cpp -- there is no on-device rendering pass to verify
against, same rationale the original file documented.

Usage: python3 generate_font.py > ../../src/font.cpp
(run from this directory; paths below are relative to this script's location)
"""
import pathlib
from PIL import Image, ImageDraw, ImageFont

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
FONT_PATH = SCRIPT_DIR / "fonts" / "Jersey10-Regular.ttf"

# (C identifier prefix, point size, extra leading px added to lineAdvance, horizontal
# spacing px appended to every glyph's advance)
SIZES = [
    ("Small", 10, 1, 1),
    ("Large", 18, 2, 1),
]

FIRST_CHAR = 0x20
LAST_CHAR = 0x7E  # printable ASCII, space..~
CHAR_CODES = list(range(FIRST_CHAR, LAST_CHAR + 1))


def char_name(code):
    ch = chr(code)
    names = {
        0x20: "space", 0x21: "bang", 0x22: "dquote", 0x23: "hash", 0x24: "dollar",
        0x25: "percent", 0x26: "amp", 0x27: "quote", 0x28: "lparen", 0x29: "rparen",
        0x2A: "star", 0x2B: "plus", 0x2C: "comma", 0x2D: "minus", 0x2E: "dot",
        0x2F: "slash", 0x3A: "colon", 0x3B: "semi", 0x3C: "lt", 0x3D: "eq", 0x3E: "gt",
        0x3F: "question", 0x40: "at", 0x5B: "lbrack", 0x5C: "backslash", 0x5D: "rbrack",
        0x5E: "caret", 0x5F: "underscore", 0x60: "backtick", 0x7B: "lbrace", 0x7C: "pipe",
        0x7D: "rbrace", 0x7E: "tilde",
    }
    if code in names:
        return names[code]
    if ch.isalnum():
        return ch
    return f"0x{code:02X}"


def rasterize_glyph(font, ch, height, pad=6):
    """Return (width, rows) -- width in px, rows[height] of '#'/'.' strings of that width.
    width is the font's own advance for ch, widened only if ink would otherwise clip."""
    adv = font.getlength(ch)
    width = max(1, round(adv))
    canvas = Image.new("L", (width + pad, height + pad), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((0, 0), ch, font=font, fill=255)
    bbox = canvas.getbbox()
    ink_right = bbox[2] if bbox else 0
    width = max(width, ink_right)
    rows = []
    for y in range(height):
        row = "".join("#" if canvas.getpixel((x, y)) >= 128 else "." for x in range(width))
        rows.append(row)
    return width, rows


def emit_size(name, size, leading, spacing):
    font = ImageFont.truetype(str(FONT_PATH), size)
    ascent, descent = font.getmetrics()
    height = ascent + descent

    widths = []
    rows_by_char = []
    for code in CHAR_CODES:
        w, rows = rasterize_glyph(font, chr(code), height)
        widths.append(w)
        rows_by_char.append(rows)
        assert len(rows) == height
        assert all(len(r) == w for r in rows)

    lines = []
    lines.append(f"// ---- kFont{name}: Jersey10-Regular @ {size}px, height={height} ----")
    lines.append(f"constexpr uint8_t k{name}Widths[{len(CHAR_CODES)}] = {{")
    for code, w in zip(CHAR_CODES, widths):
        lines.append(f"    /* '{char_name(code)}' 0x{code:02X} */ {w},")
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr uint16_t k{name}Rows[{len(CHAR_CODES)} * {height}] = {{")
    for code, rows in zip(CHAR_CODES, rows_by_char):
        lines.append(f"    /* '{char_name(code)}' 0x{code:02X} */")
        row_lits = ", ".join(f'rowBits("{r}")' for r in rows)
        # wrap long lines at ~4 rowBits() calls each for readability
        chunk = []
        parts = row_lits.split(", ")
        for i in range(0, len(parts), 4):
            chunk.append("    " + ", ".join(parts[i:i + 4]) + ("," if i + 4 < len(parts) else ","))
        lines.extend(chunk)
    lines.append("};")
    lines.append("")

    line_advance = height + leading
    font_decl = (
        f'extern const Font kFont{name} = {{ k{name}Widths, k{name}Rows, '
        f'{len(CHAR_CODES)}, \' \', {height}, {line_advance}, {spacing} }};'
    )
    return "\n".join(lines), font_decl


def main():
    header = '''\
// GENERATED FILE -- do not hand-edit. Regenerate with:
//   python3 tools/font_gen/generate_font.py > src/font.cpp
// See tools/font_gen/README.md for how to change fonts/sizes.
//
// Source font: Jersey10-Regular.ttf, Copyright 2023 The Soft Type Project Authors
// (https://github.com/scfried/soft-type-jersey), SIL Open Font License 1.1 -- full text
// in tools/font_gen/fonts/Jersey10-OFL.txt. Two sizes baked in as constexpr tables:
// kFontSmall for secondary/readout text (FPS counter, scale bar label), kFontLarge for
// titles. Each glyph keeps the font's own proportional advance width (not a fixed cell,
// unlike the original hand-drawn 5x7 font this replaces) -- see font.h for the Font
// struct/drawText() API this feeds.
//
// Row bits are still '#'/'.' ASCII art literals fed through rowBits(), same convention
// the original hand-authored table used, so each glyph's shape stays checkable by eye
// here even though the table itself is machine-generated -- there is no on-device
// rendering pass to verify against.
#include "font.h"

#include <cstddef>

#include "display.h"

template <size_t N>
constexpr uint16_t rowBits(const char (&s)[N])
{
    uint16_t bits = 0;
    for (size_t i = 0; i + 1 < N; i++)
        if (s[i] == '#')
            bits |= uint16_t(1) << (N - 2 - i);
    return bits;
}

namespace
{

'''
    body_parts = []
    decls = []
    for name, size, leading, spacing in SIZES:
        body, decl = emit_size(name, size, leading, spacing)
        body_parts.append(body)
        decls.append(decl)

    footer = '''
} // namespace

''' + "\n".join(decls) + '''

void drawChar(uint16_t *frameBuf, int x, int y, char c, uint16_t color, const Font &font)
{
    int index = int(uint8_t(c)) - int(uint8_t(font.firstChar));
    if (index < 0 || index >= font.glyphCount)
        return;
    uint8_t width = font.glyphWidths[index];
    const uint16_t *rows = font.glyphRows + size_t(index) * font.height;
    for (int row = 0; row < font.height; row++)
    {
        int py = y + row;
        if (py < 0 || py >= Display::kDisplayHeight)
            continue;
        uint16_t bits = rows[row];
        for (int col = 0; col < width; col++)
        {
            if (!(bits & (uint16_t(1) << (width - 1 - col))))
                continue;
            int px = x + col;
            if (px < 0 || px >= Display::kDisplayWidth)
                continue;
            frameBuf[py * Display::kDisplayWidth + px] = color;
        }
    }
}

static uint8_t glyphAdvance(const Font &font, char c)
{
    int index = int(uint8_t(c)) - int(uint8_t(font.firstChar));
    uint8_t w = (index >= 0 && index < font.glyphCount) ? font.glyphWidths[index] : 0;
    return w + font.spacing;
}

int drawText(uint16_t *frameBuf, int x, int y, const char *text, uint16_t color, const Font &font)
{
    int cursorX = x;
    for (const char *p = text; *p; p++)
    {
        drawChar(frameBuf, cursorX, y, *p, color, font);
        cursorX += glyphAdvance(font, *p);
    }
    return cursorX;
}

int textWidth(const char *text, const Font &font)
{
    int total = 0;
    for (const char *p = text; *p; p++)
        total += glyphAdvance(font, *p);
    return total;
}
'''
    print(header + "\n".join(body_parts) + footer)


if __name__ == "__main__":
    main()
