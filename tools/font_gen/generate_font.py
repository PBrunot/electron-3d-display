#!/usr/bin/env python3
"""Rasterize Jersey10-Regular.ttf into src/render/font_data.h's constexpr glyph tables.

Three sizes are baked in: kFontSmall (secondary/readout text -- FPS counter, scale bar
label), kFontLarge (titles), and kFontHuge (hero text -- the big element-symbol title in
atom_view.cpp, and the big shell-notation label during atom_view.cpp's dissection sequence
-- both rendered at kFontHuge's own true pixel size instead of integer-upscaling
kFontLarge, which looked blocky). Each glyph keeps its own proportional width (the font's
own advance width, not a fixed cell) -- this is what makes the result look like a real
font instead of the old fixed 5x7 grid.

This script emits ONLY glyph data (src/render/font_data.h) -- never src/render/font.h or src/render/font.cpp,
which declare/implement the actual rendering API (drawChar/drawText/etc.) by hand.
font_data.h is `#include`d exclusively by font.cpp, so regenerating it (a new font, a new
size) can never clobber hand-maintained rendering logic the way overwriting one monolithic
generated font.cpp used to risk.

Each size picks the narrowest row-storage type (ROW_CTYPE) that fits its own widest glyph,
rather than one width for every size -- kFontSmall's widest glyph fits in 8 bits, kFontLarge
needs 16, and kFontHuge's 54px glyphs (e.g. '#', '@') need up to ~35, so it uses 64. This
keeps each size's table from wasting flash on row bits it can never set.

Every glyph is rasterized in PIL mode "1" (FreeType's hinted monochrome rasterizer), not
mode "L" (8-bit antialiased grayscale) thresholded at 128 -- the two disagree on most
glyphs' trailing edge (grayscale antialiasing bleeds an extra column past what the hinter
actually commits to lighting), which is what read as a soft/aliased edge on-device instead
of the crisp pixel-font look this typeface was picked for. Rendering "at proper size" means
letting FreeType's own pixel-grid-fitted rasterizer decide each pixel, not a naive
threshold on a smoothed render.

Each glyph's row range is also trimmed to the whole charset's actual vertical ink window
(see compute_ink_window()) instead of the font's full ascent+descent box -- this charset has
no accented uppercase, so nothing ever reaches true ascent, and most glyphs never reach true
descent either, so the untrimmed box wastes several rows of blank padding above and below
every glyph. That padding is what made text drawn at a given (x, y) look like it started
several pixels lower/more spaced than (x, y) actually is.

Usage: python3 generate_font.py > ../../src/render/font_data.h
(run from this directory; paths below are relative to this script's location)
"""
import pathlib
from PIL import Image, ImageDraw, ImageFont

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
FONT_PATH = SCRIPT_DIR / "fonts" / "Jersey10-Regular.ttf"

# (C identifier prefix, point size, extra leading px added to lineAdvance, horizontal
# spacing px appended to every glyph's advance, row-storage C type)
SIZES = [
    ("Small", 10, 1, 1, "uint8_t"),
    ("Large", 18, 2, 1, "uint16_t"),
    ("Huge", 54, 4, 2, "uint64_t"),
]

FIRST_CHAR = 0x20
LAST_CHAR = 0x7E  # printable ASCII, space..~
CHAR_CODES = list(range(FIRST_CHAR, LAST_CHAR + 1))

# Each row-storage width has its own FontBase<RowT> alias in font.h, named for the size
# that actually uses it (kFontLarge keeps the plain "Font" name -- it's the one generic
# call sites like chooser.cpp/ticker.h are written against).
ROW_CTYPE_TO_FONT_ALIAS = {
    "uint8_t": "FontSmall",
    "uint16_t": "Font",
    "uint64_t": "FontHuge",
}


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


def compute_ink_window(font, full_height):
    """Return (top, bottom) -- the smallest row range that contains every printable-ASCII
    glyph's ink at this font's pixel size. Every glyph's row table is trimmed to this
    shared window (see module docstring) instead of the font's own ascent+descent box."""
    top, bottom = full_height, 0
    for code in CHAR_CODES:
        canvas = Image.new("1", (200, full_height + 20), 0)
        ImageDraw.Draw(canvas).text((0, 0), chr(code), font=font, fill=1)
        bbox = canvas.getbbox()
        if bbox:
            top = min(top, bbox[1])
            bottom = max(bottom, bbox[3])
    return top, bottom


def rasterize_glyph(font, ch, ink_top, ink_bottom, full_height, pad=6):
    """Return (width, rows) -- width in px (the font's own advance for ch, widened only if
    ink would otherwise clip) and rows[ink_bottom - ink_top] of per-row bit-ints (bit
    (width-1-col) set = that column lit, row 0 = ink_top). Rasterized in mode "1" -- see
    module docstring for why that matters vs. mode "L" thresholded at 128."""
    adv = font.getlength(ch)
    width = max(1, round(adv))
    canvas = Image.new("1", (width + pad, full_height + pad), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((0, 0), ch, font=font, fill=1)
    bbox = canvas.getbbox()
    ink_right = bbox[2] if bbox else 0
    width = max(width, ink_right)
    rows = []
    for y in range(ink_top, ink_bottom):
        bits = 0
        for x in range(width):
            if canvas.getpixel((x, y)):
                bits |= 1 << (width - 1 - x)
        rows.append(bits)
    return width, rows


def emit_size(name, size, leading, spacing, row_ctype):
    font = ImageFont.truetype(str(FONT_PATH), size)
    ascent, descent = font.getmetrics()
    full_height = ascent + descent
    ink_top, ink_bottom = compute_ink_window(font, full_height)
    height = ink_bottom - ink_top
    row_bits = {"uint8_t": 8, "uint16_t": 16, "uint32_t": 32, "uint64_t": 64}[row_ctype]
    suffix = "ULL" if row_ctype == "uint64_t" else ""

    widths = []
    rows_by_char = []
    for code in CHAR_CODES:
        w, rows = rasterize_glyph(font, chr(code), ink_top, ink_bottom, full_height)
        assert w <= row_bits, (
            f"kFont{name}: '{chr(code)}' is {w}px wide, past the {row_bits}-bit row type "
            f"{row_ctype} -- widen SIZES' row_ctype for this size"
        )
        widths.append(w)
        rows_by_char.append(rows)
        assert len(rows) == height

    # Hex digits sized to this size's own widest glyph (not row_ctype's full bit width) --
    # kFontHuge's row_ctype is 64-bit to cover a worst-case wide glyph, but most of its
    # glyphs are under 32px, so padding every literal out to 16 hex digits would just be
    # noise.
    hex_digits = max(1, (max(widths) + 3) // 4)

    lines = []
    lines.append(f"// ---- kFont{name}: Jersey10-Regular @ {size}px, height={height} ----")
    lines.append(f"constexpr uint8_t k{name}Widths[{len(CHAR_CODES)}] = {{")
    for code, w in zip(CHAR_CODES, widths):
        lines.append(f"    /* '{char_name(code)}' 0x{code:02X} */ {w},")
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr {row_ctype} k{name}Rows[{len(CHAR_CODES)} * {height}] = {{")
    for code, rows in zip(CHAR_CODES, rows_by_char):
        row_lits = ", ".join(f"0x{v:0{hex_digits}X}{suffix}" for v in rows)
        lines.append(f"    /* '{char_name(code)}' 0x{code:02X} */ {row_lits},")
    lines.append("};")
    lines.append("")

    line_advance = height + leading
    font_alias = ROW_CTYPE_TO_FONT_ALIAS[row_ctype]
    font_decl = (
        f'extern const {font_alias} kFont{name} = {{ k{name}Widths, k{name}Rows, '
        f'{len(CHAR_CODES)}, \' \', {height}, {line_advance}, {spacing} }};'
    )
    return "\n".join(lines), font_decl


def main():
    header = '''\
// GENERATED FILE -- do not hand-edit. Regenerate with:
//   python3 tools/font_gen/generate_font.py > src/render/font_data.h
// See tools/font_gen/README.md for how to change fonts/sizes.
//
// Source font: Jersey10-Regular.ttf, Copyright 2023 The Soft Type Project Authors
// (https://github.com/scfried/soft-type-jersey), SIL Open Font License 1.1 -- full text
// in tools/font_gen/fonts/Jersey10-OFL.txt.
//
// Pure glyph DATA, nothing else -- included exclusively by font.cpp, which owns the
// actual rendering logic (drawChar/drawText/etc., declared in font.h). Keeping data and
// logic in separate files means regenerating this file can never silently delete
// hand-maintained rendering code, unlike overwriting one monolithic generated font.cpp.
//
// Row bits are plain hex literals, one per pixel row per glyph (bit (width-1-col) set =
// that column lit) -- there is no on-device rendering pass to verify against, but a
// hand-inspectable ASCII-art intermediate isn't needed either: generate_font.py's mode
// "1" FreeType rasterization is the source of truth, and a rendered PNG proof (e.g.
// tools/font_gen's own preview, or an on-device screenshot) is what actually catches a bad
// glyph. Each size's row-storage type is the narrowest of uint8_t/uint16_t/uint64_t that
// fits that size's own widest glyph -- see font.h's FontBase<RowT> doc comment for why.
#pragma once

#include <cstdint>

#include "font.h"

namespace
{

'''
    body_parts = []
    decls = []
    for name, size, leading, spacing, row_ctype in SIZES:
        body, decl = emit_size(name, size, leading, spacing, row_ctype)
        body_parts.append(body)
        decls.append(decl)

    footer = '''
} // namespace

''' + "\n".join(decls) + "\n"
    print(header + "\n".join(body_parts) + footer)


if __name__ == "__main__":
    main()
