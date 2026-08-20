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
FONT_PATH = (
    SCRIPT_DIR / "fonts" / "DejaVuSans.ttf"
)  # quick test: was Jersey10-Regular.ttf
DEJAVU_PATH = SCRIPT_DIR / "fonts" / "DejaVuSans.ttf"

# (C identifier prefix, point size, extra leading px added to lineAdvance, horizontal
# spacing px appended to every glyph's advance, row-storage C type)
SIZES = [
    (
        "Small",
        8,
        1,
        1,
        "uint16_t",
    ),  # quick test: widened from uint8_t for DejaVu's wider glyphs
    (
        "Large",
        16,
        2,
        1,
        "uint32_t",
    ),  # quick test: widened from uint16_t for DejaVu's wider glyphs
    ("Huge", 40, 4, 2, "uint64_t"),
]

FIRST_CHAR = 0x20
LAST_ASCII_CHAR = 0x7E  # printable ASCII, space..~

# Two extra glyphs appended right after printable ASCII, so firstChar/glyphCount in font.h
# stays a single contiguous range. Sourced from DejaVuSans.ttf (Jersey10 has no physics
# symbols) instead of the main typeface. See font.h for the char constants strings use to
# reach these.
ELECTRON_CHAR = 0x7F  # electron symbol: e + superscript minus
SCRIPT_L_CHAR = 0x80  # script small l (U+2113): orbital angular-momentum quantum number
DEJAVU_GLYPHS = {
    ELECTRON_CHAR: "e⁻",
    SCRIPT_L_CHAR: "ℓ",
}

LAST_CHAR = SCRIPT_L_CHAR
CHAR_CODES = list(range(FIRST_CHAR, LAST_CHAR + 1))

# Each SIZES entry has its own FontBase<RowT> alias in font.h, named for the size that
# actually uses it (kFontLarge keeps the plain "Font" name -- it's the one generic call
# sites like chooser.cpp/ticker.h are written against). Keyed by name, not row_ctype --
# quick-test font swaps can widen two sizes to the same row_ctype without colliding.
NAME_TO_FONT_ALIAS = {
    "Small": "FontSmall",
    "Large": "Font",
    "Huge": "FontHuge",
}


def char_name(code):
    ch = chr(code)
    names = {
        0x20: "space",
        0x21: "bang",
        0x22: "dquote",
        0x23: "hash",
        0x24: "dollar",
        0x25: "percent",
        0x26: "amp",
        0x27: "quote",
        0x28: "lparen",
        0x29: "rparen",
        0x2A: "star",
        0x2B: "plus",
        0x2C: "comma",
        0x2D: "minus",
        0x2E: "dot",
        0x2F: "slash",
        0x3A: "colon",
        0x3B: "semi",
        0x3C: "lt",
        0x3D: "eq",
        0x3E: "gt",
        0x3F: "question",
        0x40: "at",
        0x5B: "lbrack",
        0x5C: "backslash",
        0x5D: "rbrack",
        0x5E: "caret",
        0x5F: "underscore",
        0x60: "backtick",
        0x7B: "lbrace",
        0x7C: "pipe",
        0x7D: "rbrace",
        0x7E: "tilde",
        ELECTRON_CHAR: "electron",
        SCRIPT_L_CHAR: "script_l",
    }
    if code in names:
        return names[code]
    if ch.isalnum():
        return ch
    return f"0x{code:02X}"


# Headroom (px) added above AND below every glyph's nominal ascent-top before drawing, so a
# DejaVu glyph whose ascent exceeds Jersey10's own (see baseline_offset in emit_size, which
# can shift a glyph up by drawing it at a negative y) still has canvas above row 0 to draw
# into instead of getting silently clipped by the canvas edge -- as opposed to clipped by
# the intentional [ink_top, ink_bottom) crop below, which is fine. Both functions below
# must draw at this same origin for their ink_top/ink_bottom coordinates to agree.
Y_ORIGIN = 24


def compute_ink_window(glyph_specs, full_height):
    """Return (top, bottom) -- the smallest row range that contains every glyph's ink at
    this font's pixel size. glyph_specs is a list of (font, text, stroke_width) triples
    (text is a codepoint's char, or a DejaVu string for the two special glyphs;
    stroke_width fakes a bold weight, see DEJAVU_STROKE_WIDTH). Every glyph's row table is
    trimmed to this shared window (see module docstring) instead of the font's own
    ascent+descent box.
    """
    top, bottom = full_height + Y_ORIGIN, 0
    for font, text, stroke_width in glyph_specs:
        canvas = Image.new("1", (200, full_height + 2 * Y_ORIGIN), 0)
        ImageDraw.Draw(canvas).text(
            (stroke_width, Y_ORIGIN), text, font=font, fill=1, stroke_width=stroke_width
        )
        bbox = canvas.getbbox()
        if bbox:
            top = min(top, bbox[1])
            bottom = max(bottom, bbox[3])
    return top, bottom


def rasterize_glyph(
    font, text, ink_top, ink_bottom, full_height, stroke_width=0, y_offset=0, pad=6
):
    """Return (width, rows) -- width in px (the font's own advance for text, widened only
    if ink would otherwise clip) and rows[ink_bottom - ink_top] of per-row bit-ints (bit
    (width-1-col) set = that column lit, row 0 = ink_top). text is usually one codepoint's
    char, but the two DejaVu glyphs pass a short string (e.g. "e" + superscript minus)
    rasterized as a single glyph cell. stroke_width outlines the glyph to fake a bold
    weight (see DEJAVU_STROKE_WIDTH) -- drawn stroke_width px right of the canvas origin so
    a left-side stroke doesn't clip off the edge. y_offset shifts the glyph (relative to
    Y_ORIGIN, same as compute_ink_window's draw position) before cropping to [ink_top,
    ink_bottom) -- needed for the two DejaVu glyphs, whose font has a different ascent than
    Jersey10 at the same nominal draw position, so drawing both at the same y leaves them
    baseline-mismatched (see emit_size's baseline_offset). Rasterized in mode "1" -- see
    module docstring for why that matters vs. mode "L" thresholded at 128."""
    adv = font.getlength(text)
    width = max(1, round(adv)) + stroke_width
    canvas = Image.new("1", (width + pad, full_height + 2 * Y_ORIGIN), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text(
        (stroke_width, Y_ORIGIN + y_offset),
        text,
        font=font,
        fill=1,
        stroke_width=stroke_width,
    )
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


def fit_dejavu_font(target_size, row_bits, jersey_ascent, ink_top):
    """Largest DejaVu instance (point size <= target_size) that (a) fits every DEJAVU_GLYPHS
    string inside row_bits px, and (b) once baseline-aligned to Jersey10 (see emit_size's
    baseline_offset), doesn't poke ink above ink_top. The electron glyph ("e" + superscript
    minus) is wider than Jersey10's own widest glyph at kFontSmall/kFontLarge's point sizes;
    script-l's ascender loop is *taller* than Jersey10's own tallest glyph at every size,
    DejaVu being a conventional typeface where an ascender reaches close to true ascent,
    unlike pixel-font Jersey10's own shorter glyphs -- so shrinking DejaVu until its ascender
    clears ink_top (rather than growing the ASCII-derived window, see below) is what keeps
    script-l's loop intact without reflowing the other 95 glyphs.
    """
    for s in range(target_size, 0, -1):
        f = ImageFont.truetype(str(DEJAVU_PATH), s)
        if not all(f.getlength(t) <= row_bits for t in DEJAVU_GLYPHS.values()):
            continue
        dejavu_ascent, _ = f.getmetrics()
        baseline_offset = jersey_ascent - dejavu_ascent
        canvas = Image.new(
            "1", (row_bits + 8, f.getmetrics()[0] + f.getmetrics()[1] + 2 * Y_ORIGIN), 0
        )
        ImageDraw.Draw(canvas).text(
            (0, Y_ORIGIN + baseline_offset),
            DEJAVU_GLYPHS[SCRIPT_L_CHAR],
            font=f,
            fill=1,
        )
        bbox = canvas.getbbox()
        if bbox is None or bbox[1] >= ink_top:
            return f
    return ImageFont.truetype(str(DEJAVU_PATH), 1)


def emit_size(name, size, leading, spacing, row_ctype):
    font = ImageFont.truetype(str(FONT_PATH), size)
    row_bits = {"uint8_t": 8, "uint16_t": 16, "uint32_t": 32, "uint64_t": 64}[row_ctype]
    ascent, descent = font.getmetrics()

    # Ink window is computed from the Jersey10/ASCII glyphs only (independent of the DejaVu
    # glyphs below), so all 95 existing glyphs' row tables (and on-screen vertical position)
    # stay byte-identical to before this script knew about DEJAVU_GLYPHS -- a wider window
    # to fit script-l's tall ascender in full would otherwise reflow every ASCII glyph by a
    # few px for the sake of two rarely-drawn symbols. fit_dejavu_font() instead shrinks the
    # DejaVu instance until its own ascender clears this window.
    ink_top, ink_bottom = compute_ink_window(
        [(font, chr(c), 0) for c in range(FIRST_CHAR, LAST_ASCII_CHAR + 1)],
        ascent + descent,
    )
    height = ink_bottom - ink_top
    suffix = "ULL" if row_ctype == "uint64_t" else ""

    dejavu_font = fit_dejavu_font(size, row_bits, ascent, ink_top)
    dejavu_ascent, dejavu_descent = dejavu_font.getmetrics()
    full_height = max(ascent + descent, dejavu_ascent + dejavu_descent)
    # PIL draws text with its font's own ascent-top at the given y, so Jersey10 and DejaVu
    # -- different fonts at different point sizes, with different ascent metrics -- don't
    # share a baseline when both are drawn at the same y. Shifting the DejaVu draw by this
    # offset aligns its baseline to Jersey's, so the same ink_top/ink_bottom window crops
    # both fonts consistently (see fit_dejavu_font(), which already picked a DejaVu size
    # small enough for this alignment to not clip script-l's ascender).
    baseline_offset = ascent - dejavu_ascent

    def spec(code):
        if code in DEJAVU_GLYPHS:
            return dejavu_font, DEJAVU_GLYPHS[code], 0, baseline_offset
        return font, chr(code), 0, 0

    widths = []
    rows_by_char = []
    for code in CHAR_CODES:
        glyph_font, text, stroke_width, y_offset = spec(code)
        w, rows = rasterize_glyph(
            glyph_font, text, ink_top, ink_bottom, full_height, stroke_width, y_offset
        )
        assert w <= row_bits, (
            f"kFont{name}: '{text}' is {w}px wide, past the {row_bits}-bit row type "
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
    lines.append(
        f"// ---- kFont{name}: Jersey10-Regular @ {size}px, height={height} ----"
    )
    lines.append(f"constexpr uint8_t k{name}Widths[{len(CHAR_CODES)}] = {{")
    for code, w in zip(CHAR_CODES, widths):
        lines.append(f"    /* '{char_name(code)}' 0x{code:02X} */ {w},")
    lines.append("};")
    lines.append("")
    lines.append(
        f"constexpr {row_ctype} k{name}Rows[{len(CHAR_CODES)} * {height}] = {{"
    )
    for code, rows in zip(CHAR_CODES, rows_by_char):
        row_lits = ", ".join(f"0x{v:0{hex_digits}X}{suffix}" for v in rows)
        lines.append(f"    /* '{char_name(code)}' 0x{code:02X} */ {row_lits},")
    lines.append("};")
    lines.append("")

    line_advance = height + leading
    font_alias = NAME_TO_FONT_ALIAS[name]
    font_decl = (
        f"extern const {font_alias} kFont{name} = {{ k{name}Widths, k{name}Rows, "
        f"{len(CHAR_CODES)}, ' ', {height}, {line_advance}, {spacing} }};"
    )
    return "\n".join(lines), font_decl


def main():
    header = """\
// GENERATED FILE -- do not hand-edit. Regenerate with:
//   python3 tools/font_gen/generate_font.py > src/render/font_data.h
// See tools/font_gen/README.md for how to change fonts/sizes.
//
// Source font: Jersey10-Regular.ttf, Copyright 2023 The Soft Type Project Authors
// (https://github.com/scfried/soft-type-jersey), SIL Open Font License 1.1 -- full text
// in tools/font_gen/fonts/Jersey10-OFL.txt. Two glyphs (electron symbol, script-l) are
// sourced from DejaVuSans.ttf instead, since Jersey10 has no physics symbols.
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

"""
    body_parts = []
    decls = []
    for name, size, leading, spacing, row_ctype in SIZES:
        body, decl = emit_size(name, size, leading, spacing, row_ctype)
        body_parts.append(body)
        decls.append(decl)

    footer = """
} // namespace

""" + "\n".join(decls) + "\n"
    print(header + "\n".join(body_parts) + footer)


if __name__ == "__main__":
    main()
