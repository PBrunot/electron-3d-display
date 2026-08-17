# font_gen

Offline generator for `src/font.cpp` (see that file's header comment for the on-device
API/format). Rasterizes a real typeface into `constexpr` glyph tables at build time on a
PC -- nothing is decoded or rasterized on the ESP32 itself, same "precompute offline,
embed as `.rodata`" pattern used elsewhere in this project (e.g. `orbitals.h`).

## Regenerating

```sh
python3 generate_font.py > ../../src/font.cpp
```

Requires Pillow (`pip install pillow`).

## Current font

`fonts/Jersey10-Regular.ttf` -- a pixel-grid monospace-ish font (Copyright 2023 The Soft
Type Project Authors, https://github.com/scfried/soft-type-jersey), SIL Open Font License
1.1 (full text: `fonts/Jersey10-OFL.txt`). Chosen for the "crisp pixel/mono" look while
still having genuinely distinct lowercase letterforms (several other tiny pixel fonts,
e.g. Silkscreen, fall back to uppercase shapes for lowercase at small sizes -- checked
during evaluation, ruled out for that reason).

Two sizes are baked into `src/font.cpp`:
- `kFontSmall` (10px) -- secondary/readout text: FPS counter, scale bar label.
- `kFontLarge` (18px) -- titles (element/orbital name, electron configuration).

Both cover printable ASCII (space through `~`, 0x20-0x7E). Each glyph keeps its own
proportional advance width (the font's own metric, not a fixed cell) -- `generate_font.py`
widens a glyph's stored width past its nominal advance only if the rendered ink would
otherwise clip, so nothing is cut off.

## Changing the font or sizes

Edit the `SIZES` list at the top of `generate_font.py` (name, point size, extra line
leading, horizontal spacing) and/or point `FONT_PATH` at a different `.ttf`/`.otf`, then
regenerate. Any font Pillow can load with `ImageFont.truetype()` works. Check the new
font's license before committing it here -- `LICENSE`-adjacent files in this directory
exist so each vendored font's terms travel with the binary it produced.

## Why offline rasterization instead of a runtime font library

See the CLAUDE.md-adjacent design discussion this came out of: on-device options like
LVGL (needs its own render/tick loop) or u8g2's compact binary font format (real custom
~500-line variable-bit-width decoder, more risk than benefit for a project with no
on-device way to verify a mis-ported decoder) were both heavier or riskier than rasterizing
once on a PC and shipping the result as a plain, directly-inspectable bitmap table --
consistent with how every other precomputed dataset in this repo is generated.
