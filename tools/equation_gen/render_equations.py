#!/usr/bin/env python3
"""Rasterize this project's Schroedinger/psi_real formulas into a 1-bit bitmap, then emit
them as a generated C header+source (src/equation_bitmap.h/.cpp) for OrbitalView's
quantum-number reveal animation (see atom_view.cpp's scrollElementIntro() for the sibling
"name intro" pattern this backdrop sits under, and src/orbitals.h's psiReal() docstring for
where the second formula comes from -- this is a straight transcription, not a re-derivation).

Renders as an IMAGE via matplotlib mathtext rather than extending the on-device font system
(src/font.h is 8-bit-char/ASCII-only, and Jersey10-Regular.ttf -- the source typeface for
that font, see tools/font_gen/ -- has none of psi/theta/phi/nabla anyway, confirmed via
fontTools). This keeps the font system untouched; the equations are just a static background
image, not live text.

Regenerate with: python3 tools/equation_gen/render_equations.py
"""
import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Target on-device size. Kept comfortably under the 240px display width so the backdrop has
# margin on both sides; height sized for two stacked lines.
WIDTH_PX = 216
HEIGHT_PX = 80
DPI = 100

# Line 1: general time-independent Schroedinger equation (no existing vetted transcription
# in-repo -- see README.md's "The math behind it" section, which stops at psi_nlm = R*Y and
# never writes this one out -- so this is the standard textbook form, not a project port).
# Line 2: README.md's ORBITALI.md's psi_nlm = R_nl(r) * Y_l^m(theta, phi) is the general
# form; what this project's src/orbitals.h::psiReal() actually EVALUATES is R_nl(r) *
# P_l^|m|(theta) * (cos(m*phi) for m>=0, sin(|m|*phi) for m<0) -- see that function's own
# docstring. Abbreviated to "trig(m*phi)" here (both branches spelled out would need a
# piecewise/cases construct matplotlib's mathtext subset doesn't reliably support) since
# this is background decoration, not a full derivation.
LINES = [
    r"$\hat{H}\psi = E\psi$",
    r"$\psi_{n\ell m} = R_{n\ell}(r)\,P_\ell^{|m|}(\theta)\,\mathrm{trig}(m\phi)$",
]

OUT_H = "/mnt/d/GitHub/electron-3d-display/src/equation_bitmap.h"
OUT_CPP = "/mnt/d/GitHub/electron-3d-display/src/equation_bitmap.cpp"


def rasterize():
    fig = plt.figure(figsize=(WIDTH_PX / DPI, HEIGHT_PX / DPI), dpi=DPI)
    fig.patch.set_alpha(0.0)
    for i, line in enumerate(LINES):
        y = 0.72 - i * 0.44
        fig.text(0.5, y, line, ha="center", va="center", fontsize=11, color="white")
    fig.canvas.draw()
    buf = np.asarray(fig.canvas.buffer_rgba())  # HEIGHT_PXxWIDTH_PXx4, alpha channel = ink
    plt.close(fig)
    alpha = buf[:, :, 3]
    mask = alpha > 96  # binarize -- True where a glyph was drawn
    return mask


def emit(mask):
    height, width = mask.shape
    row_bytes = (width + 7) // 8
    packed = bytearray(height * row_bytes)
    for y in range(height):
        for x in range(width):
            if mask[y, x]:
                packed[y * row_bytes + (x // 8)] |= 0x80 >> (x % 8)

    with open(OUT_H, "w") as f:
        f.write(
            "\n".join(
                [
                    "// GENERATED FILE -- do not hand-edit. Regenerate with",
                    "// tools/equation_gen/render_equations.py. See that script for what this",
                    "// bitmap shows and why it's an image instead of on-device font glyphs.",
                    "#pragma once",
                    "",
                    "#include <cstdint>",
                    "",
                    f"constexpr int kEquationBitmapWidth = {width};",
                    f"constexpr int kEquationBitmapHeight = {height};",
                    f"constexpr int kEquationBitmapRowBytes = {row_bytes};",
                    "extern const uint8_t kEquationBitmapData[];",
                    "",
                    "/** Blit the equation backdrop, top-left at (x, y), OR'ing `color` into frameBuf",
                    " * wherever a bit is set and leaving every other pixel untouched (so it composites",
                    " * under other text/points already drawn, and under whatever's drawn after it,",
                    " * matching this project's usual \"caller clears first\" draw-function contract).",
                    " * Bounds-checked like every other direct-pixel draw in this project. */",
                    "void drawEquationBackdrop(uint16_t *frameBuf, int x, int y, uint16_t color);",
                    "",
                ]
            )
        )

    with open(OUT_CPP, "w") as f:
        lines = [
            "// GENERATED FILE -- do not hand-edit. See equation_bitmap.h.",
            '#include "equation_bitmap.h"',
            "",
            '#include "display.h"',
            "",
            "const uint8_t kEquationBitmapData[] = {",
        ]
        for y in range(height):
            row = packed[y * row_bytes : (y + 1) * row_bytes]
            lines.append("    " + ",".join(f"0x{b:02X}" for b in row) + ",")
        lines.append("};")
        lines.append("")
        lines.append("void drawEquationBackdrop(uint16_t *frameBuf, int x, int y, uint16_t color) {")
        lines.append("    for (int row = 0; row < kEquationBitmapHeight; row++) {")
        lines.append("        int py = y + row;")
        lines.append("        if (py < 0 || py >= Display::kDisplayHeight)")
        lines.append("            continue;")
        lines.append("        for (int col = 0; col < kEquationBitmapWidth; col++) {")
        lines.append("            int px = x + col;")
        lines.append("            if (px < 0 || px >= Display::kDisplayWidth)")
        lines.append("                continue;")
        lines.append(
            "            uint8_t byte = kEquationBitmapData[row * kEquationBitmapRowBytes + col / 8];"
        )
        lines.append("            if (byte & (0x80 >> (col % 8)))")
        lines.append("                frameBuf[py * Display::kDisplayWidth + px] = color;")
        lines.append("        }")
        lines.append("    }")
        lines.append("}")
        lines.append("")
        f.write("\n".join(lines))

    print(f"wrote {OUT_H} and {OUT_CPP}: {width}x{height}px, {len(packed)} bytes packed")


if __name__ == "__main__":
    emit(rasterize())
