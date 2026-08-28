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
over via resample()), N_POINTS (cloud_common.N_POINTS for orbitals; atom_view.py
sets its own -- both match the C++ S3 build's production point counts), the
run() loop and its input handling (both
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
ZOOM_ANGLE_STEP = 0.016  # breathing zoom speed -- atom_view.py's value (kZoomAngleStep);
                          # orbital_view.py uses ORBITAL_ZOOM_ANGLE_STEP instead.
ORBITAL_ZOOM_ANGLE_STEP = ZOOM_ANGLE_STEP / 1.5  # kOrbitalZoomAngleStep
TILT_ANGLE_STEP = 0.023   # X-axis rotation speed, kept close to ANGLE_STEP so tilt/roll don't
                           # lag behind yaw and leave axis-aligned lobes looking frozen.
ROLL_ANGLE_STEP = 0.017   # Z-axis rotation speed -- required (not cosmetic) so no point stays
                           # screen-invariant; see orbital_view.py's "why three axes" note.
_TILT_ANGLE_START = 0.9   # tilt/roll start off the degenerate all-zero pose, so frame 1
_ROLL_ANGLE_START = 2.1   # isn't axis-locked.

# Fly-over (fly_over()): ease from base_scale*factor to base_scale over `frames` frames.
# These are atom_view.py's own values (kIntroFrames/kSwitchTransitionFrames); orbital_view.py
# uses its own slower ORBITAL_* counterparts instead.
INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
ORBITAL_INTRO_FRAMES = 105  # kOrbitalIntroFrames
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 35
ORBITAL_SWITCH_TRANSITION_FRAMES = 27  # kOrbitalSwitchTransitionFrames

# Random zoom excursions: at randomized intervals, ease to a random scale and back, layered
# over the steady breathing sine wave. Bounds are shared atom/orbital; only the ease-frame
# count differs (ZOOM_EXCURSION_EASE_FRAMES is atom_view.py's; orbital_view.py uses
# ORBITAL_ZOOM_EXCURSION_EASE_FRAMES).
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 150
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 500
ZOOM_EXCURSION_SCALE_MIN_FACTOR = 0.3
ZOOM_EXCURSION_SCALE_MAX_FACTOR = 10.0
ZOOM_EXCURSION_EASE_FRAMES = 45
ORBITAL_ZOOM_EXCURSION_EASE_FRAMES = 68

PROTON_SIZE = 3            # drawn before the cloud so points can blend over it
PROMINENT_PROTON_SIZE = 5  # redrawn opaque on top after the cloud, so it's never obscured

# fade_buffer()/render_points() implement the real C++ fade+blend math (bit-exact when
# enabled), but default to disabled (0/256) since the full-frame fade costs ~3x the frame time
# here. Set to 160/240 to match C++ exactly.
PERSISTENCE_KEEP_Q8 = 0
ELECTRON_ALPHA_Q8 = 256

TITLE_TEXT_POS = (1, 1)  # matches kTitleTextX/kTitleTextY
LOADING_TEXT = "Loading..."
LOADING_TEXT_POS = (2, 22)

# framebuf's font is a fixed 8x8 bitmap with no size argument on this build --
# draw_text_scaled() fakes scaling by rendering each glyph into an 8x8 scratch buffer, then
# nearest-neighbor-blitting it at scale*8px (_blit_glyph_scaled()). These approximate C++'s
# three font sizes (9/17/42px) -- HUGE stays below the closest integer multiple (40px, scale 5)
# since this font is monospace (glyphs always 8px wide) where C++'s is proportional, so the
# SAME nominal size reads visibly bulkier/heavier here at equal height; scale 4 trades a bit of
# height-match for weight closer to C++'s actual look.
FONT_SCALE_SMALL = 1  # 8px
FONT_SCALE_LARGE = 2  # 16px
FONT_SCALE_HUGE = 4   # 32px

# Idle-timeout auto-cycling (kChooserIdleJumpUs/kViewIdleJumpUs), in ms since
# time.ticks_ms() is this platform's clock.
CHOOSER_IDLE_JUMP_MS = 30_000
VIEW_IDLE_JUMP_MS = 60_000

# Bottom-left physical-size reference bar -- device (framebuf) counterpart of
# pc/viewer_common.draw_scale_bar(), same geometry as src/render/overlay.cpp's drawScaleBar()
# (kScaleBarMarginX/Y, kScaleBarMaxPx, kScaleBarTickPx, kScaleBarLabelGapPx,
# kScaleBarLineThicknessPx) so a bar reads the same length/prominence on both platforms. The
# "nice round length" ladder itself (cloud_common.pick_scale_bar_length()) is shared too.
SCALE_BAR_MARGIN_X = 16
SCALE_BAR_MARGIN_Y = 26  # a few px more than kScaleBarMarginY's 16 -- this panel's bottom edge
                          # crowds the bar/ticks slightly more than on the C++ side's build
SCALE_BAR_MAX_PX = 180
SCALE_BAR_TICK_PX = 8
SCALE_BAR_LABEL_GAP_PX = 4
SCALE_BAR_LINE_THICKNESS_PX = 2

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


BOUNDING_CIRCLE_COLOR = encode_color565(180, 180, 180)  # matches kBoundingCircleColor


# Scratch 8x8 glyph buffer, shared across every draw_text_scaled() call (not one per call) to
# avoid per-frame GC pressure, since text is drawn every frame.
_GLYPH_BUF = bytearray(8 * 8 * 2)
_GLYPH_FB = framebuf.FrameBuffer(_GLYPH_BUF, 8, 8, framebuf.RGB565)


@micropython.viper
def _blit_glyph_scaled(dst_buf, glyph_buf, dst_x: int, dst_y: int, scale: int, dst_w: int, dst_h: int):
    """Nearest-neighbor-replicate the 8x8 glyph in `glyph_buf` into `dst_buf` at (dst_x, dst_y),
    rotated 180 degrees (row,col -> 7-row,7-col) -- same panel orientation fix render_points()
    applies inline, done here in the same per-pixel loop rather than a separate pass. Each
    source pixel becomes a scale x scale block; background (0) is transparent, same as
    framebuf.text().
    """
    pdst = ptr16(dst_buf)
    pglyph = ptr16(glyph_buf)
    row = 0
    while row < 8:
        col = 0
        while col < 8:
            c = pglyph[row * 8 + col]
            if c != 0:
                base_x = dst_x + (7 - col) * scale
                base_y = dst_y + (7 - row) * scale
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


def pick_text_scale(s, max_width, max_scale=FONT_SCALE_HUGE, min_scale=FONT_SCALE_SMALL):
    """Largest scale in [min_scale, max_scale] where `s` still fits max_width -- the font is
    monospace (see text_width_scaled()), so a fixed huge scale that fits a short label like "Fe"
    can badly overflow a long one like "3d_x2-y2". Falls back to min_scale if nothing fits.
    """
    scale = max_scale
    while scale > min_scale and text_width_scaled(s, scale) > max_width:
        scale -= 1
    return scale


def draw_text_scaled(fb, buf, x, y, s, color, scale):
    """Draw `s` at `scale`x framebuf's native 8x8 font, sprite-space top-left (x, y) -- same
    convention as fb.text() and every other coordinate in this project (see module docstring's
    "Overlays" note). Each glyph is rendered into the shared 8x8 scratch buffer (_GLYPH_FB),
    then nearest-neighbor-blitted into `buf` rotated 180 degrees (_blit_glyph_scaled()) at the
    panel's physical position -- characters are drawn in REVERSE order, starting from the
    string's own flipped bounding box, since rotating the whole string 180 degrees reverses
    reading order too (each glyph's pixels are individually rotated; composing that with
    reversed draw order is what makes the whole string read correctly, not upside down or
    scrambled). Costs an extra round-trip per character vs. fb.text() directly, so callers that
    only want the native 8px size AND don't need this project's orientation fix (rare -- see
    fb.text() call sites) should call fb.text() instead.
    """
    total_width = len(s) * 8 * scale
    height = 8 * scale
    phys_x = WIDTH - x - total_width
    phys_y = HEIGHT - y - height
    cursor_x = phys_x
    for ch in reversed(s):
        _GLYPH_FB.fill(0)
        _GLYPH_FB.text(ch, 0, 0, color)
        _blit_glyph_scaled(buf, _GLYPH_BUF, cursor_x, phys_y, scale, WIDTH, HEIGHT)
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
    """Per-point levels/signs -> encoded RGB565 array, for orbital_view.py's PresetState (and
    benchmark_test.py). cloud_common.level_to_rgb()'s scale and encode_color565()'s
    color565()+swap16() are inlined rather than called, since a call from native-compiled code
    still pays full bytecode overhead unless the callee is native too.
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
    """Per-point (r, g, b) tuples -> encoded RGB565 array, for atom_view.py's AtomPresetState
    (and benchmark_test.py) -- same inlined color565()+swap16() as encode_orbital_colors().
    """
    n = len(rgb_list)
    colors = array.array('H', bytes(2 * n))
    for i in range(n):
        r, g, b = rgb_list[i]
        native565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        colors[i] = ((native565 & 0xFF) << 8) | (native565 >> 8)
    return colors


def draw_scale_bar(fb, buf, pixels_per_unit, unit_label, bar_color, text_color, max_bar_px=SCALE_BAR_MAX_PX):
    """Device (framebuf) counterpart of src/render/overlay.cpp's drawScaleBar() -- same "nice
    round length" ladder (cloud_common.pick_scale_bar_length()), same geometry (bar + two end
    ticks, SCALE_BAR_LINE_THICKNESS_PX thick, drawn via fb.fill_rect() for thickness -- fb.hline/
    vline are always 1px), and draw_text_scaled() at FONT_SCALE_LARGE for the label.
    pixels_per_unit <= 0 draws nothing (defensive only).
    """
    if pixels_per_unit <= 0:
        return
    length, label = cloud_common.pick_scale_bar_length(pixels_per_unit, max_bar_px)
    bar_px = max(1, int(length * pixels_per_unit))

    x0 = SCALE_BAR_MARGIN_X
    y = HEIGHT - SCALE_BAR_MARGIN_Y
    x1 = x0 + bar_px
    t = SCALE_BAR_LINE_THICKNESS_PX

    # Rectangles are symmetric under a 180-degree rotation, so only their POSITION needs the
    # panel-orientation fix (no per-pixel work, unlike text) -- same w-1-x/h-1-y remap
    # render_points()/the proton marker use, applied to each rect's flipped top-left corner.
    fb.fill_rect(WIDTH - x0 - bar_px, HEIGHT - y - t, bar_px, t, bar_color)
    tick_h = 2 * SCALE_BAR_TICK_PX + 1
    fb.fill_rect(WIDTH - x0 - t, HEIGHT - y - SCALE_BAR_TICK_PX - 1, t, tick_h, bar_color)
    fb.fill_rect(WIDTH - x1 - t, HEIGHT - y - SCALE_BAR_TICK_PX - 1, t, tick_h, bar_color)

    label_height = 8 * FONT_SCALE_LARGE
    draw_text_scaled(fb, buf, x0, y - SCALE_BAR_TICK_PX - SCALE_BAR_LABEL_GAP_PX - label_height,
                     "%s %s" % (label, unit_label), text_color, FONT_SCALE_LARGE)


@micropython.viper
def fade_buffer(buf, w: int, h: int, keep_q8: int):
    """Full-frame persistence fade -- MicroPython port of Display::fade(): each pixel is
    expanded to 8-bit-per-channel (bit-replication, matching Display::unpackColor565() exactly,
    not a plain left-shift), scaled by keep_q8/256, and truncated back to 5/6/5. `buf` holds
    byte-swapped RGB565 (see module docstring), so each value is un/re-swapped around the math.
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

    Each written pixel is alpha-blended toward its target color (read, blend at alpha_q8/256,
    write back) -- MicroPython port of Display::blendColor565(), same expand/truncate as
    fade_buffer(). See render_points_opaque() below for the plain-overwrite fast path.
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
    """render_points()'s ELECTRON_ALPHA_Q8=256 fast path: same rotate/project/buzz, but a plain
    overwrite instead of read+blend+write. Exact, not an approximation -- blendColor565() at
    alpha=256 reduces algebraically to the target color unchanged.
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
    """Fade (or clear, see PERSISTENCE_KEEP_Q8), draw the proton marker small before the cloud
    (blendable), render every point in `preset`, then redraw the marker bigger and opaque on
    top so it's never hidden under a point -- matches src/render/camera.h's renderScene() plus
    each C++ view's own post-cloud marker redraw. `preset` needs xs_fx/ys_fx/zs_fx/colors.
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

    prominent_x = CENTER - PROMINENT_PROTON_SIZE // 2
    prominent_y = CENTER - PROMINENT_PROTON_SIZE // 2
    prominent_radius = PROMINENT_PROTON_SIZE // 2
    fb.ellipse(w1 - prominent_x + prominent_radius, h1 - prominent_y + prominent_radius, prominent_radius,
               prominent_radius, proton_color, True)

    preset.draw_bounding_circle(fb, buf, scale)


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
