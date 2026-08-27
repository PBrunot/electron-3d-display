"""Render/camera helpers shared by micropython/orbital_view.py and
micropython/atom_view.py. Both are ESP32-S3 firmware apps showing a tumbling
Q8-fixed-point/viper point cloud on the ST7789 panel, with the same camera
model (yaw/tilt/roll advance, intro/switch fly-overs, random zoom
excursions), the same proton marker/scale-bar overlays, and the same
buf->framebuf->blit_buffer() pipeline -- this module is that common layer,
mirroring pc/viewer_common.py's role on the PC side (extracted for the same
reason: so atom_view.py doesn't have to reach into orbital_view.py's
internals for it).

What stays OUT of this module, in each app instead: the per-app PresetState
class (PresetState/AtomPresetState -- different data sources and coloring:
cloud_common phase-by-sign vs atom_cloud shell-by-n, only PresetState turns
over via resample()), N_POINTS (cloud_common.N_POINTS=3000 for orbitals;
atom_view.py sets its own), the run() loop and its input handling (both
cycle via the same nudge gesture, but orbital_view.py wraps a fixed preset
index while atom_view.py clamps an atomic number), and FPS-counter
bookkeeping (dev/debug only, trivial enough not to share).
"""

import array
import math
import random
import time

import cloud_common
import display as display_mod
import framebuf
import st7789py as st7789

try:
    import nudge
    import qmi8658
except ImportError:
    nudge = None
    qmi8658 = None

WIDTH = display_mod.WIDTH
HEIGHT = display_mod.HEIGHT
CENTER = WIDTH // 2

FX_BITS = 8
FX_SCALE = 1 << FX_BITS  # Q8 fixed-point scale factor, see orbital_view.py's module docstring

ANGLE_STEP = 0.030
FRAME_DELAY_MS = 5
ZOOM_ANGLE_STEP = 0.016  # breathing zoom's angular speed; independent phase from ANGLE_STEP
TILT_ANGLE_STEP = 0.023   # second (X-axis) rotation's angular speed. Kept close to ANGLE_STEP
                           # (not much slower) on purpose: with tilt=roll=0, a point's screen-Y
                           # depends only on tilt+roll, NOT on yaw at all -- so if tilt/roll lag
                           # far behind yaw, axis-aligned lobes (e.g. 3d_x2-y2's) sit still for
                           # the first second or two while yaw visibly spins everything else,
                           # reading as "a fixed axis that doesn't rotate" even though it does
                           # eventually. Non-resonant vs. ANGLE_STEP/ROLL_ANGLE_STEP so the
                           # tumble doesn't fall into a short repeating loop.
ROLL_ANGLE_STEP = 0.017   # third (Z-axis) rotation's angular speed -- required, not cosmetic,
                           # see orbital_view.py's module docstring's "why three axes, not two".
                           # Also kept close to ANGLE_STEP for the same "don't lag behind yaw"
                           # reason.
_TILT_ANGLE_START = 0.9   # tilt_angle/roll_angle start away from the degenerate all-zero pose
_ROLL_ANGLE_START = 2.1   # (where yaw alone can't move axis-aligned lobes at all), so even
                           # the first frame after boot isn't axis-locked

# Fly-over (see fly_over()): camera starts at base_scale * factor and eases
# to base_scale over `frames` frames. Boot intro is slower/more dramatic
# than a preset/element switch, which is more dramatic than a mid-scene zoom
# excursion.
INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 18

# Random zoom excursions during the steady-state loop: at randomized
# intervals (re-rolled after each one, so the cadence itself isn't
# periodic), ease from the current breathing scale to a randomized target
# and back -- layered on top of the constant sine-wave breathing so the
# animation doesn't read as purely mechanical.
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 150
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 400
ZOOM_EXCURSION_SCALE_MIN_FACTOR = 0.4
ZOOM_EXCURSION_SCALE_MAX_FACTOR = 5.0
ZOOM_EXCURSION_EASE_FRAMES = 30

PROTON_SIZE = 3

# Persistence-fade / alpha-blend constants -- MicroPython port of src/config/visual_constants.h's
# kPersistenceKeepQ8/kElectronAlphaQ8 (both /256 fixed-point): when non-default (see below),
# src/render/camera.h's renderScene()/renderSceneGrouped() fade the WHOLE frame buffer toward
# black every frame (not a hard clear) and alpha-blend every point write against whatever's
# already there (not an opaque overwrite) -- fade_buffer()/render_points() below are the
# MicroPython port of both, bit-identical to the C++ formulas when enabled.
#
# Disabled by default here (0 / 256), UNLIKE the C++ build, which keeps both on
# (kPersistenceKeepQ8=160, kElectronAlphaQ8=240): per BENCHMARK.md's MicroPython section, the
# full-frame fade touches WIDTH*HEIGHT pixels every frame regardless of point count and is the
# dominant per-frame cost on this interpreted target -- with fade+blend matching C++ bit-for-bit,
# MicroPython was consistently ~3x slower at every point count, not just build/sampling-bound.
# render_frame() below takes the fast path when these are at their disabled values (fb.fill(0)
# instead of fade_buffer()'s per-pixel loop; render_points_opaque()'s plain overwrite instead of
# render_points()'s per-point read+blend) -- actual CPU saved, not just a different-looking
# no-op. Both real implementations are left intact and still exactly match the C++ math, so
# setting these back to 160/240 (e.g. to re-run the parity comparison against
# src/debug/benchmark_test.cpp via micropython/benchmark_test.py) restores bit-identical,
# apples-to-apples behavior -- these two just no longer describe the MicroPython viewers'
# shipped default.
PERSISTENCE_KEEP_Q8 = 0    # 0 = disabled (render_frame() does fb.fill(0)); 160 matches kPersistenceKeepQ8
ELECTRON_ALPHA_Q8 = 256    # 256 = disabled (render_frame() does an opaque overwrite); 240 matches kElectronAlphaQ8

# Overlays are drawn in panel-native (non-prism-corrected) orientation --
# to_physical() is a coordinate remap, not a glyph-rotation, so framebuf
# text can't be made readable through the prism offset anyway.
#
# (1, 1), not the old (2, 12) -- matches src/config/visual_constants.h's
# kTitleTextX/kTitleTextY exactly, now that titles are drawn at FONT_SCALE_HUGE
# (see below) instead of framebuf's bare 8px font, so the old offset (tuned
# for that smaller size) left too much dead margin above/left of the title.
TITLE_TEXT_POS = (1, 1)
LOADING_TEXT = "Loading..."
LOADING_TEXT_POS = (2, 22)

# framebuf's built-in font is a fixed 8x8 bitmap with no size parameter on
# this MicroPython build (confirmed on-device: FrameBuffer.text() rejects a
# 6th positional arg) -- draw_text_scaled() below fakes a scaled font by
# rendering each glyph into an 8x8 scratch buffer, then nearest-neighbor
# blitting it into the destination at scale*8 px per glyph (see
# _blit_glyph_scaled()). These three integer scales approximate the C++
# side's three baked font sizes (kFontSmall=9px/kFontLarge=17px/
# kFontHuge=42px, src/render/font_data.h) closely enough to read as "small/
# large/huge" on this same 240x240 panel without chasing an exact pixel
# match to a completely different (proportional, hand-rasterized) font
# system -- kFontHuge's true 42px would eat a big fraction of this panel's
# height for even a 2-character title; 32px (scale 4) leaves more headroom
# while still reading clearly larger than "large".
FONT_SCALE_SMALL = 1  # 8px -- unchanged from framebuf's native size
FONT_SCALE_LARGE = 2  # 16px -- ~kFontLarge
FONT_SCALE_HUGE = 4   # 32px -- ~kFontHuge (scaled down from its true 42px, see above)

# Idle-timeout auto-cycling, MicroPython counterpart of
# src/config/visual_constants.h's kChooserIdleJumpUs (chooser.py)/
# kViewIdleJumpUs (orbital_view.py/atom_view.py) -- milliseconds, not
# microseconds, since time.ticks_ms() (not esp_timer_get_time()) is this
# platform's monotonic clock.
CHOOSER_IDLE_JUMP_MS = 30_000
VIEW_IDLE_JUMP_MS = 60_000

# Bottom-left physical-size reference bar -- device (framebuf) counterpart
# of pc/viewer_common.draw_scale_bar(), same geometry/margins (panel is the
# same 240x240 as the PC debug window's un-upscaled buffer) so a bar reads
# the same physical length on both renderers at a given zoom. The
# "nice round length" ladder itself (cloud_common.pick_scale_bar_length())
# is shared too -- see draw_scale_bar() below for the framebuf drawing.
SCALE_BAR_MARGIN_X = 8
SCALE_BAR_MARGIN_Y = 8
SCALE_BAR_MAX_PX = 90
SCALE_BAR_TICK_PX = 4

# Direction -> index/Z step, within a running viewer. Only L/R cycle now --
# U is reserved (see NUDGE_BACK_DIRECTION below) to return to chooser.py's
# menu, and D is currently unused (free for a future gesture). The old
# mapping had R/U both advance and L/D both go back, fully redundant since
# nudge.py's axis calibration was still a placeholder when it was chosen;
# dropping U/D here costs nothing real (L/R alone already cover advance and
# go-back) and frees U for a real "back" gesture.
_NUDGE_DIRECTION_STEP = {'R': 1, 'L': -1}

# Nudging this direction from within orbital_view.py/atom_view.py's run()
# loop returns to chooser.py's menu instead of stepping the preset/element
# -- checked BEFORE _NUDGE_DIRECTION_STEP (that dict no longer has an entry
# for it, so the two checks can't both match the same direction anyway, but
# being explicit here is clearer than relying on that omission).
NUDGE_BACK_DIRECTION = 'U'


def random_index_excluding(current, count):
    """Random int in [0, count), guaranteed != current -- MicroPython port
    of src/render/camera.cpp's randomIndexExcluding(), same offset trick
    (pick a random step in [1, count-1] and wrap) so it can never land back
    on `current`. `count` must be >= 2.
    """
    offset = 1 + random.randrange(count - 1)
    return (current + offset) % count


def swap16(color565):
    return ((color565 & 0xFF) << 8) | (color565 >> 8)


def encode_color565(r, g, b):
    return swap16(st7789.color565(r, g, b))


# Scratch 8x8 glyph buffer, reused across every draw_text_scaled() call --
# framebuf's built-in font has no size parameter on this build (see
# FONT_SCALE_* above), so each character is rendered at its native 8x8 size
# into this tiny scratch buffer first, then _blit_glyph_scaled() below
# nearest-neighbor-replicates it into the destination at scale*8 px. One
# shared buffer, not one per call: text is drawn every frame (title/scale
# bar/labels), so allocating a fresh bytearray+FrameBuffer per glyph per
# frame would be needless per-frame GC pressure.
_GLYPH_BUF = bytearray(8 * 8 * 2)
_GLYPH_FB = framebuf.FrameBuffer(_GLYPH_BUF, 8, 8, framebuf.RGB565)


@micropython.viper
def _blit_glyph_scaled(dst_buf, glyph_buf, dst_x: int, dst_y: int, scale: int, dst_w: int, dst_h: int):
    """Nearest-neighbor-replicate the 8x8 glyph in `glyph_buf` into
    `dst_buf` (dst_w x dst_h RGB565) at (dst_x, dst_y), each source pixel
    becoming a scale x scale block. Background (0 -- _GLYPH_FB.fill(0)
    before each glyph) is treated as transparent, so only the glyph's own
    "on" pixels are written -- letting scaled text composite over whatever
    was already drawn, same as framebuf.text()'s own transparent-background
    behavior at scale 1.
    """
    pdst = ptr16(dst_buf)
    pglyph = ptr16(glyph_buf)
    row = 0
    while row < 8:
        col = 0
        while col < 8:
            c = pglyph[row * 8 + col]
            if c != 0:
                base_x = dst_x + col * scale
                base_y = dst_y + row * scale
                by = 0
                while by < scale:
                    py = base_y + by
                    if 0 <= py < dst_h:
                        row_off = py * dst_w
                        bx = 0
                        while bx < scale:
                            px = base_x + bx
                            if 0 <= px < dst_w:
                                pdst[row_off + px] = c
                            bx += 1
                    by += 1
            col += 1
        row += 1


def text_width_scaled(s, scale):
    """Pixel width of `s` drawn via draw_text_scaled() at `scale` --
    framebuf's font is monospace 8px/glyph, so unlike C++'s proportional
    font (textWidth()/textWidthScaled(), font.cpp) this needs no per-glyph
    width table, just len(s)*8*scale.
    """
    return len(s) * 8 * scale


def draw_text_scaled(fb, buf, x, y, s, color, scale):
    """Draw `s` at `scale`x framebuf's native 8x8 font (see FONT_SCALE_*
    above), left-to-right from (x, y), by rendering each glyph into the
    shared 8x8 scratch buffer (_GLYPH_FB) and nearest-neighbor-blitting it
    into `buf` (the raw bytearray backing `fb`) at scale*8 px --
    draw_text_scaled(..., scale=1) is NOT the same call as fb.text(...)
    directly (an extra glyph-buffer round-trip per character), so callers
    that only ever want the native 8px size (e.g. the FPS counter) should
    keep calling fb.text() directly instead.
    """
    cursor_x = x
    for ch in s:
        _GLYPH_FB.fill(0)
        _GLYPH_FB.text(ch, 0, 0, color)
        _blit_glyph_scaled(buf, _GLYPH_BUF, cursor_x, y, scale, WIDTH, HEIGHT)
        cursor_x += 8 * scale
    return cursor_x - x


@micropython.native
def to_fixed(values):
    out = array.array('i', bytes(4 * len(values)))
    for i in range(len(values)):
        out[i] = int(values[i] * FX_SCALE)
    return out


@micropython.native
def encode_orbital_colors(levels, signs, phase_pair):
    """Per-point levels/signs -> encoded RGB565 array, for
    orbital_view.py's PresetState (and micropython/benchmark_test.py).

    cloud_common.level_to_rgb()'s per-channel scale and
    encode_color565()'s color565()+swap16() are inlined here rather than
    called, even though this function is itself @micropython.native --
    calling a NON-native function from native-compiled code still pays the
    interpreter's full bytecode call overhead for that call (native
    compilation only speeds up the caller's OWN body), so leaving those as
    real calls would have left most of the per-point cost unaffected.
    Inlining lets the whole per-point body run as native-compiled integer
    arithmetic with no bytecode call-outs at all.
    """
    n = len(levels)
    colors = array.array('H', bytes(2 * n))
    pos_r, pos_g, pos_b = phase_pair[0]
    neg_r, neg_g, neg_b = phase_pair[1]
    for i in range(n):
        level = levels[i]
        if signs[i] >= 0:
            r = pos_r * level // 255
            g = pos_g * level // 255
            b = pos_b * level // 255
        else:
            r = neg_r * level // 255
            g = neg_g * level // 255
            b = neg_b * level // 255
        native565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        colors[i] = ((native565 & 0xFF) << 8) | (native565 >> 8)
    return colors


@micropython.native
def encode_rgb_colors(rgb_list):
    """Per-point (r, g, b) tuples -> encoded RGB565 array, for
    atom_view.py's AtomPresetState (and benchmark_test.py) -- same
    inlined-color565()+swap16() reasoning as encode_orbital_colors() above.
    """
    n = len(rgb_list)
    colors = array.array('H', bytes(2 * n))
    for i in range(n):
        r, g, b = rgb_list[i]
        native565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        colors[i] = ((native565 & 0xFF) << 8) | (native565 >> 8)
    return colors


def draw_scale_bar(fb, buf, pixels_per_unit, unit_label, bar_color, text_color, max_bar_px=SCALE_BAR_MAX_PX):
    """Device (framebuf) counterpart of pc/viewer_common.draw_scale_bar() --
    same "nice round length" ladder (cloud_common.pick_scale_bar_length(),
    which also supplies each length's precomputed display string, so this
    never needs '%g'-style float formatting), drawn with fb.hline()/
    fb.vline() for the bar itself and draw_text_scaled() (FONT_SCALE_LARGE,
    matching src/render/overlay.cpp's drawScaleBar() -- kFontLarge "at its
    own true size, not kFontSmall integer-upscaled") for the label. Panel-
    native (non-prism-corrected) coordinates, same convention as the title/
    corner-label text it sits next to. pixels_per_unit <= 0 draws nothing
    (defensive only -- scale is never <= 0 in normal operation).
    """
    if pixels_per_unit <= 0:
        return
    length, label = cloud_common.pick_scale_bar_length(pixels_per_unit, max_bar_px)
    bar_px = max(1, int(length * pixels_per_unit))

    x0 = SCALE_BAR_MARGIN_X
    y = HEIGHT - SCALE_BAR_MARGIN_Y
    x1 = x0 + bar_px

    fb.hline(x0, y, bar_px, bar_color)
    fb.vline(x0, y - SCALE_BAR_TICK_PX, 2 * SCALE_BAR_TICK_PX + 1, bar_color)
    fb.vline(x1, y - SCALE_BAR_TICK_PX, 2 * SCALE_BAR_TICK_PX + 1, bar_color)
    label_height = 8 * FONT_SCALE_LARGE
    draw_text_scaled(fb, buf, x0, y - SCALE_BAR_TICK_PX - 4 - label_height, "%s %s" % (label, unit_label),
                     text_color, FONT_SCALE_LARGE)


@micropython.viper
def fade_buffer(buf, w: int, h: int, keep_q8: int):
    """Full-frame persistence fade -- MicroPython port of Display::fade()
    (src/render/display.cpp), which src/render/camera.h's renderScene()/
    renderSceneGrouped() call every frame INSTEAD of a hard clear. Each
    pixel's RGB565 is expanded to 8-bit-per-channel (same `(v5<<3)|(v5>>2)`/
    `(v6<<2)|(v6>>4)` bit-replication Display::unpackColor565() uses -- not
    a plain left-shift, which would leave the low bits always zero and bias
    the faded color darker than the C++ side's), scaled by keep_q8/256,
    then truncated back to 5/6/5 (Display::packColor565()'s plain `>>3`/
    `>>2`/`>>3`, no rounding, matching that function exactly). `buf` holds
    byte-swapped RGB565 (see module docstring's "Byte-order gotcha") so
    each value is un-swapped before this and re-swapped after -- same
    convention render_points() below already uses per point.
    """
    pbuf = ptr16(buf)
    n = w * h
    i = 0
    while i < n:
        v = pbuf[i]
        native = ((v & 0xFF) << 8) | (v >> 8)
        r5 = (native >> 11) & 0x1F
        g6 = (native >> 5) & 0x3F
        b5 = native & 0x1F
        r8 = ((r5 << 3) | (r5 >> 2)) * keep_q8 >> 8
        g8 = ((g6 << 2) | (g6 >> 4)) * keep_q8 >> 8
        b8 = ((b5 << 3) | (b5 >> 2)) * keep_q8 >> 8
        native2 = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
        pbuf[i] = ((native2 & 0xFF) << 8) | (native2 >> 8)
        i += 1


@micropython.viper
def render_points(buf, xs, ys, zs, colors, n: int,
                   cos_y_fx: int, sin_y_fx: int, cos_x_fx: int, sin_x_fx: int,
                   cos_z_fx: int, sin_z_fx: int, scale_fx: int,
                   cx: int, cy: int, w: int, h: int, frame_salt: int, buzz_threshold: int,
                   alpha_q8: int):
    """Rotate (yaw about Y, tilt about X, roll about Z -- all three needed,
    see orbital_view.py's module docstring), project, and draw every point
    directly into `buf` -- Q8 fixed-point only (viper can't do float->int
    here). Shift amounts (8, 16) are literal ints, not the FX_BITS constant
    -- referencing a module global from inside viper yields a boxed
    'object', which can't mix with viper's native int arithmetic; keep in
    sync with FX_BITS by hand if it ever changes.

    Depth after yaw (rz1) is computed only to feed the tilt step, then
    dropped -- rendering stays depth-sort-free (see CLAUDE.md section 5), so
    the post-tilt/post-roll depth is never needed either.

    Every `>> N` here is preceded by `+ (1 << (N-1))` (128 for the >>8 steps,
    32768 for the final >>16): plain `>>` on a signed int is a floor (rounds
    toward -inf), not round-to-nearest, so without the offset every stage
    would be systematically biased low. The float-side equivalent of this
    (`int()` truncating toward zero in pc/viewer_common.py's render_frame)
    was measured to bias points about 6-15% toward the screen's horizontal/
    vertical centerlines vs. the diagonals -- fixed there with round();
    this is the fixed-point way to remove the same class of bias.

    "Buzz" (see BUZZ_FRACTION in cloud_common.py): hv is a cheap
    multiplicative hash (668265261/374761393 -- Bob Jenkins'/xxHash's
    32-bit constants, picked over the more common 2654435761 because that
    one is >= 2^31 and doesn't fit viper's native int literal) of the point
    index and frame_salt, taking the high 16 bits (low bits of a
    multiplicative hash distribute poorly). buzz_threshold=0 disables it
    (atom_view.py passes 0 -- no per-frame flicker for the static atom
    cloud).

    Each written pixel is alpha-blended toward its target color (read the
    existing pixel, blend at alpha_q8/256, write back) rather than an
    opaque overwrite -- MicroPython port of Display::blendColor565()
    (src/render/display.cpp), same bit-replication expand/truncate as
    fade_buffer() above. This is real per-point work C++'s
    renderPointsColored()/renderPointsGrouped() also do every point, not
    optional polish: skipping it (a plain overwrite) makes overlapping
    points fully replace each other instead of visually accumulating.
    """
    pxs = ptr32(xs)
    pys = ptr32(ys)
    pzs = ptr32(zs)
    pcolors = ptr16(colors)
    pbuf = ptr16(buf)
    i = 0
    while i < n:
        hv = ((i * 668265261 + frame_salt * 374761393) >> 16) & 0xFFFF
        if hv >= buzz_threshold:
            x = pxs[i]
            y = pys[i]
            z = pzs[i]
            rx1 = (x * cos_y_fx + z * sin_y_fx + 128) >> 8
            rz1 = (z * cos_y_fx - x * sin_y_fx + 128) >> 8
            ry2 = (y * cos_x_fx - rz1 * sin_x_fx + 128) >> 8
            rx3 = (rx1 * cos_z_fx - ry2 * sin_z_fx + 128) >> 8
            ry3 = (rx1 * sin_z_fx + ry2 * cos_z_fx + 128) >> 8
            sx = cx + ((rx3 * scale_fx + 32768) >> 16)
            sy = cy - ((ry3 * scale_fx + 32768) >> 16)
            if sx >= 0 and sx < w and sy >= 0 and sy < h:
                idx = (h - 1 - sy) * w + (w - 1 - sx)

                old = pbuf[idx]
                old_native = ((old & 0xFF) << 8) | (old >> 8)
                or5 = (old_native >> 11) & 0x1F
                og6 = (old_native >> 5) & 0x3F
                ob5 = old_native & 0x1F
                or8 = (or5 << 3) | (or5 >> 2)
                og8 = (og6 << 2) | (og6 >> 4)
                ob8 = (ob5 << 3) | (ob5 >> 2)

                tgt = pcolors[i]
                tgt_native = ((tgt & 0xFF) << 8) | (tgt >> 8)
                tr5 = (tgt_native >> 11) & 0x1F
                tg6 = (tgt_native >> 5) & 0x3F
                tb5 = tgt_native & 0x1F
                tr8 = (tr5 << 3) | (tr5 >> 2)
                tg8 = (tg6 << 2) | (tg6 >> 4)
                tb8 = (tb5 << 3) | (tb5 >> 2)

                r8 = or8 + (((tr8 - or8) * alpha_q8) >> 8)
                g8 = og8 + (((tg8 - og8) * alpha_q8) >> 8)
                b8 = ob8 + (((tb8 - ob8) * alpha_q8) >> 8)

                native2 = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
                pbuf[idx] = ((native2 & 0xFF) << 8) | (native2 >> 8)
        i += 1


@micropython.viper
def render_points_opaque(buf, xs, ys, zs, colors, n: int,
                         cos_y_fx: int, sin_y_fx: int, cos_x_fx: int, sin_x_fx: int,
                         cos_z_fx: int, sin_z_fx: int, scale_fx: int,
                         cx: int, cy: int, w: int, h: int, frame_salt: int, buzz_threshold: int):
    """render_points()'s ELECTRON_ALPHA_Q8=256 fast path (see module docstring):
    same rotation/projection/buzz as render_points() above, but writes each
    point's color as a plain overwrite -- no old-pixel read, no unpack/blend/
    pack. Correct, not just "close enough": blendColor565()'s formula at
    alpha_q8=256 reduces algebraically to the target color unchanged
    (`or8 + ((tr8-or8)*256>>8) == tr8`), and since `colors[]` is already
    stored byte-swapped the same as `buf` (see render_points()'s own
    unpack/pack round trip), the blended-then-repacked result is just
    `pcolors[i]` again -- so skipping straight to `pbuf[idx] = pcolors[i]`
    below is exact, not an approximation.
    """
    pxs = ptr32(xs)
    pys = ptr32(ys)
    pzs = ptr32(zs)
    pcolors = ptr16(colors)
    pbuf = ptr16(buf)
    i = 0
    while i < n:
        hv = ((i * 668265261 + frame_salt * 374761393) >> 16) & 0xFFFF
        if hv >= buzz_threshold:
            x = pxs[i]
            y = pys[i]
            z = pzs[i]
            rx1 = (x * cos_y_fx + z * sin_y_fx + 128) >> 8
            rz1 = (z * cos_y_fx - x * sin_y_fx + 128) >> 8
            ry2 = (y * cos_x_fx - rz1 * sin_x_fx + 128) >> 8
            rx3 = (rx1 * cos_z_fx - ry2 * sin_z_fx + 128) >> 8
            ry3 = (rx1 * sin_z_fx + ry2 * cos_z_fx + 128) >> 8
            sx = cx + ((rx3 * scale_fx + 32768) >> 16)
            sy = cy - ((ry3 * scale_fx + 32768) >> 16)
            if sx >= 0 and sx < w and sy >= 0 and sy < h:
                idx = (h - 1 - sy) * w + (w - 1 - sx)
                pbuf[idx] = pcolors[i]
        i += 1


def render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, frame_salt=0,
                  buzz_threshold=0):
    """Fade (NOT clear -- see PERSISTENCE_KEEP_Q8/fade_buffer() above), draw
    the proton marker (via framebuf -- cheap, not once-per-point), then
    every point in `preset` at `angle`/`tilt_angle`/`roll_angle`/`scale`,
    alpha-blended (ELECTRON_ALPHA_Q8, see render_points()). `preset` need
    only expose xs_fx/ys_fx/zs_fx/colors (PresetState and AtomPresetState
    both do). Shared by fly_over() and each app's steady-state loop.

    PERSISTENCE_KEEP_Q8/ELECTRON_ALPHA_Q8 are checked once per frame here
    (not per-pixel/per-point) to pick the fast disabled-state path
    (fb.fill(0), render_points_opaque()) vs. the real fade/blend -- see
    those constants' module-level comment.
    """
    w1 = WIDTH - 1
    h1 = HEIGHT - 1

    if PERSISTENCE_KEEP_Q8 == 0:
        fb.fill(0)
    else:
        fade_buffer(buf, WIDTH, HEIGHT, PERSISTENCE_KEEP_Q8)
    proton_x = CENTER - PROTON_SIZE // 2
    proton_y = CENTER - PROTON_SIZE // 2
    proton_radius = PROTON_SIZE // 2
    fb.ellipse(w1 - proton_x + proton_radius, h1 - proton_y + proton_radius, proton_radius, proton_radius,
               proton_color, True)

    cos_y_fx = int(math.cos(angle) * FX_SCALE)
    sin_y_fx = int(math.sin(angle) * FX_SCALE)
    cos_x_fx = int(math.cos(tilt_angle) * FX_SCALE)
    sin_x_fx = int(math.sin(tilt_angle) * FX_SCALE)
    cos_z_fx = int(math.cos(roll_angle) * FX_SCALE)
    sin_z_fx = int(math.sin(roll_angle) * FX_SCALE)
    scale_fx = int(scale * FX_SCALE)
    if ELECTRON_ALPHA_Q8 == 256:
        render_points_opaque(buf, preset.xs_fx, preset.ys_fx, preset.zs_fx, preset.colors, len(preset.xs_fx),
                             cos_y_fx, sin_y_fx, cos_x_fx, sin_x_fx, cos_z_fx, sin_z_fx, scale_fx,
                             CENTER, CENTER, WIDTH, HEIGHT, frame_salt, buzz_threshold)
    else:
        render_points(buf, preset.xs_fx, preset.ys_fx, preset.zs_fx, preset.colors, len(preset.xs_fx),
                      cos_y_fx, sin_y_fx, cos_x_fx, sin_x_fx, cos_z_fx, sin_z_fx, scale_fx,
                      CENTER, CENTER, WIDTH, HEIGHT, frame_salt, buzz_threshold, ELECTRON_ALPHA_Q8)


def fly_over(d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
             start_scale, end_scale, frames, buzz_threshold=0):
    """Ease the projection scale from start_scale to end_scale over `frames`
    frames, rendering+blitting each one. Shared by the boot intro,
    nudge-triggered switches, and random zoom excursions. Returns the
    running (angle, tilt_angle, roll_angle) so rotation continues smoothly
    afterward. `buzz_threshold` (see render_points()) defaults to 0 (no
    flicker) -- orbital_view.py passes its BUZZ_FRACTION-derived threshold
    explicitly so the "buzz" effect stays active through transitions too,
    matching this code's pre-refactor behavior; atom_view.py's static cloud
    has no use for it and leaves it at the default.

    Title text is drawn via `preset.draw_title(fb, x, y, text_color)`, not a
    plain `fb.text(preset.title, ...)` call -- PresetState's title is one
    plain string, but AtomPresetState's is several segments each colored by
    shell (mirrors pc/atom_view_pc.py's draw_atom_title()), which a single
    fb.text() call can't express. Delegating to the preset keeps this
    function agnostic to which kind it's driving.
    """
    two_pi = 2 * math.pi
    for i in range(frames):
        t = i / (frames - 1) if frames > 1 else 1.0
        scale = start_scale + (end_scale - start_scale) * t
        render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, i, buzz_threshold)
        preset.draw_title(fb, buf, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        preset.draw_corner_label(fb, buf, text_color)
        draw_scale_bar(fb, buf, scale / cloud_common.PM_PER_BOHR, "pm", scale_bar_color, text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
        angle += ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi
        time.sleep_ms(FRAME_DELAY_MS)
    return angle, tilt_angle, roll_angle


def init_nudge_detector(label="switching"):
    """Best-effort IMU + nudge detector setup; None (with a printed warning)
    if the QMI8658 isn't present/answering -- an optional feature failing
    shouldn't take the animation down with it. `label` only changes the log
    text (e.g. "orbital switching" vs "element switching").
    """
    if qmi8658 is None or nudge is None:
        print("nudge: qmi8658/nudge modules not found, nudge-controlled %s disabled" % label)
        return None
    try:
        imu = qmi8658.QMI8658()
        detector = nudge.NudgeDetector(imu)
        print("nudge: QMI8658 ready, nudge-controlled %s enabled" % label)
        return detector
    except OSError as e:
        print("nudge: IMU init failed (%s), nudge-controlled %s disabled" % (e, label))
        return None


def next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)
