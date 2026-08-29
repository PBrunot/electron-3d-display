#!/usr/bin/env python3
"""RETIRED -- no longer regenerates anything. src/render/splash_bitmap.h/.cpp are hand-written
now: the device decodes img/atomic_cube.jpg (mirrored at data/atomic_cube.jpg, deployed to the
"storage" SPIFFS partition) on-device at runtime via the ROM's TJpgDec decoder, instead of
embedding the pre-decoded pixels this script used to produce. Kept only as a record of that old
approach -- do not run this against the current splash_bitmap.h/.cpp, it will overwrite the
hand-written decoder with a stale generated array.

Original docstring, for the old approach's rationale:

Convert img/atomic_cube.jpg into a generated C header+source (src/render/splash_bitmap.h/.cpp)
holding it as a raw, already-panel-packed RGB565 pixel array -- the boot splash screen shown
by main.cpp before the tilt calibration/chooser flow starts.

Decodes the JPG ONCE here, offline, and embeds the raw pixels, rather than adding a JPEG
decoder library + decode step to the firmware: this project already uses that same
offline-precompute pattern for tools/equation_gen/render_equations.py's equation backdrop
and the orbital/atom point clouds (CLAUDE.md section 5), the splash is a single static
240x240 image (no decoder needed for anything ELSE onboard), and a raw blit at boot is both
the simplest and the fastest possible path (a plain memcpy, no decode-time cost at all).

Pixels are packed through the exact same bit formula as Display::packColor565() (src/render/
display.h) -- plain textbook RGB565 (see CLAUDE.md section 2 for why: this panel's real
quirk is a missing esp_lcd data_endian/rgb_ele_order config, not a G/B bit-swap needed in
software here) -- so the emitted array is ALREADY in this panel's native format;
drawSplashScreen() just copies bytes, no per-pixel packing on-device.

Regenerate with: python3 tools/splash_gen/render_splash.py
"""
from PIL import Image

SRC_IMAGE = "/mnt/d/GitHub/electron-3d-display/img/atomic_cube.jpg"
OUT_H = "/mnt/d/GitHub/electron-3d-display/src/render/splash_bitmap.h"
OUT_CPP = "/mnt/d/GitHub/electron-3d-display/src/render/splash_bitmap.cpp"

# Matches Display::kDisplayWidth/kDisplayHeight (src/render/display.h) -- the splash is drawn as one
# opaque full-frame blit, not a positioned/composited backdrop like equation_bitmap.h's.
WIDTH_PX = 240
HEIGHT_PX = 240


def pack_color565(r, g, b):
    """Bit-for-bit port of Display::packColor565() (src/render/display.h) -- MUST stay identical to
    that formula. Plain textbook RGB565; the panel-specific correction lives in the esp_lcd
    config (display.cpp), not in this bit layout."""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def load_pixels():
    im = Image.open(SRC_IMAGE).convert("RGB")
    if im.size != (WIDTH_PX, HEIGHT_PX):
        # Center-crop-then-resize would preserve aspect better, but the source is already
        # exactly 240x240 (matches the display 1:1) -- plain resize is a no-op in that case
        # and a reasonable fallback otherwise, not worth more machinery for a single fixed asset.
        im = im.resize((WIDTH_PX, HEIGHT_PX), Image.LANCZOS)
    return list(im.getdata())  # row-major, (r, g, b) tuples


def emit(pixels):
    packed = [pack_color565(r, g, b) for (r, g, b) in pixels]

    with open(OUT_H, "w") as f:
        f.write(
            "\n".join(
                [
                    "// GENERATED FILE -- do not hand-edit. Regenerate with",
                    "// tools/splash_gen/render_splash.py. See that script for why this is a",
                    "// raw pre-packed pixel array instead of an on-device JPEG decode.",
                    "#pragma once",
                    "",
                    "#include <cstdint>",
                    "",
                    f"constexpr int kSplashBitmapWidth = {WIDTH_PX};",
                    f"constexpr int kSplashBitmapHeight = {HEIGHT_PX};",
                    "// Already packed via Display::packColor565()'s exact bit formula (plain",
                    "// textbook RGB565) -- draw with a plain copy, never re-pack these values.",
                    "extern const uint16_t kSplashBitmapData[];",
                    "",
                    "/** Blit the splash image at (0, 0), opaque (no blending) -- caller presents the",
                    " * frame afterward. Bounds-checked against Display::kDisplayWidth/Height like",
                    " * every other draw function in this project, though the emitted size always",
                    " * matches the display exactly (see WIDTH_PX/HEIGHT_PX in the generator). */",
                    "void drawSplashScreen(uint16_t *frameBuf);",
                    "",
                ]
            )
        )

    with open(OUT_CPP, "w") as f:
        lines = [
            "// GENERATED FILE -- do not hand-edit. See splash_bitmap.h.",
            '#include "splash_bitmap.h"',
            "",
            '#include <cstring>',
            "",
            '#include "display.h"',
            "",
            "const uint16_t kSplashBitmapData[] = {",
        ]
        for y in range(HEIGHT_PX):
            row = packed[y * WIDTH_PX : (y + 1) * WIDTH_PX]
            lines.append("    " + ",".join(f"0x{v:04X}" for v in row) + ",")
        lines.append("};")
        lines.append("")
        lines.append("void drawSplashScreen(uint16_t *frameBuf) {")
        lines.append("    if (kSplashBitmapWidth == Display::kDisplayWidth && kSplashBitmapHeight == Display::kDisplayHeight) {")
        lines.append("        std::memcpy(frameBuf, kSplashBitmapData, sizeof(kSplashBitmapData));")
        lines.append("        return;")
        lines.append("    }")
        lines.append("    for (int row = 0; row < kSplashBitmapHeight && row < Display::kDisplayHeight; row++)")
        lines.append("        std::memcpy(frameBuf + row * Display::kDisplayWidth, kSplashBitmapData + row * kSplashBitmapWidth,")
        lines.append("                    (kSplashBitmapWidth < Display::kDisplayWidth ? kSplashBitmapWidth : Display::kDisplayWidth) * sizeof(uint16_t));")
        lines.append("}")
        lines.append("")
        f.write("\n".join(lines))

    print(f"wrote {OUT_H} and {OUT_CPP}: {WIDTH_PX}x{HEIGHT_PX}px, {len(packed) * 2} bytes raw")


if __name__ == "__main__":
    emit(load_pixels())
