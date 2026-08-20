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

# Overlays are drawn in panel-native (non-prism-corrected) orientation --
# to_physical() is a coordinate remap, not a glyph-rotation, so framebuf
# text can't be made readable through the prism offset anyway; these are
# dev/debug text, not worth the effort.
TITLE_TEXT_POS = (2, 12)
LOADING_TEXT = "Loading..."
LOADING_TEXT_POS = (2, 22)

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


def swap16(color565):
    return ((color565 & 0xFF) << 8) | (color565 >> 8)


def encode_color565(r, g, b):
    return swap16(st7789.color565(r, g, b))


def to_fixed(values):
    out = array.array('i', bytes(4 * len(values)))
    for i in range(len(values)):
        out[i] = int(values[i] * FX_SCALE)
    return out


def draw_scale_bar(fb, pixels_per_unit, unit_label, bar_color, text_color, max_bar_px=SCALE_BAR_MAX_PX):
    """Device (framebuf) counterpart of pc/viewer_common.draw_scale_bar() --
    same "nice round length" ladder (cloud_common.pick_scale_bar_length(),
    which also supplies each length's precomputed display string, so this
    never needs '%g'-style float formatting), drawn with fb.hline()/
    fb.vline()/fb.text() instead of PIL. Panel-native (non-prism-corrected)
    coordinates, same convention as the title/FPS text it sits next to (see
    module docstring's "Overlays are drawn..." note). pixels_per_unit <= 0
    draws nothing (defensive only -- scale is never <= 0 in normal
    operation).
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
    fb.text("%s %s" % (label, unit_label), x0, y - SCALE_BAR_TICK_PX - 12, text_color)


@micropython.viper
def render_points(buf, xs, ys, zs, colors, n: int,
                   cos_y_fx: int, sin_y_fx: int, cos_x_fx: int, sin_x_fx: int,
                   cos_z_fx: int, sin_z_fx: int, scale_fx: int,
                   cx: int, cy: int, w: int, h: int, frame_salt: int, buzz_threshold: int):
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
                pbuf[(h - 1 - sy) * w + (w - 1 - sx)] = pcolors[i]
        i += 1


def render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, frame_salt=0,
                  buzz_threshold=0):
    """Clear, draw the proton marker (via framebuf -- cheap, not
    once-per-point), then every point in `preset` at `angle`/`tilt_angle`/
    `roll_angle`/`scale`. `preset` need only expose xs_fx/ys_fx/zs_fx/colors
    (PresetState and AtomPresetState both do). Shared by fly_over() and each
    app's steady-state loop.
    """
    w1 = WIDTH - 1
    h1 = HEIGHT - 1

    fb.fill(0)
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
    render_points(buf, preset.xs_fx, preset.ys_fx, preset.zs_fx, preset.colors, len(preset.xs_fx),
                  cos_y_fx, sin_y_fx, cos_x_fx, sin_x_fx, cos_z_fx, sin_z_fx, scale_fx,
                  CENTER, CENTER, WIDTH, HEIGHT, frame_salt, buzz_threshold)


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
        preset.draw_title(fb, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        draw_scale_bar(fb, scale / cloud_common.PM_PER_BOHR, "pm", scale_bar_color, text_color)
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
