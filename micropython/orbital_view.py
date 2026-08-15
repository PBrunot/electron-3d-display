"""Hydrogen orbital point-cloud animation for the ESP32-S3 panel. Orbital
math/sampling/ranking/point-turnover logic lives in cloud_common.py, shared
with pc/orbital_view_pc.py -- this module adds everything hardware-specific:
Q8 fixed-point + viper rendering, framebuf/ST7789 display, nudge/IMU input.

Rendering: orthographic projection, three-axis tumble (yaw about Y, tilt
about X, roll about Z -- each its own running angle, all at different
non-resonant rates, see ANGLE_STEP/TILT_ANGLE_STEP/ROLL_ANGLE_STEP) applied
in sequence each frame, no depth-sort/z-buffer (fine for a sparse cloud, see
CLAUDE.md section 5). Double-buffered via framebuf.FrameBuffer +
ST7789.blit_buffer().

Why three axes, not two: yaw+tilt alone (an earlier version of this code)
still left a real artifact -- a point's SCREEN-X coordinate, after yaw then
tilt, works out to `x*cos(yaw) + z*sin(yaw)` alone (tilt is a rotation about
X, which by definition never changes a point's X coordinate). So any point
near the world Y axis (small x, z -- e.g. the core of a 2p_y or 3d_x2-y2
lobe) stays pinned to the vertical screen centerline for *every* combination
of yaw and tilt, no matter how long you wait -- a persistent streak, not a
transient one. Two single-axis rotations only ever span a 2-parameter
subfamily of SO(3); a third independent axis (roll) is what actually
guarantees no point is invariant in any screen coordinate (verified: adding
roll makes that same near-Y-axis point's screen-X vary freely).

Byte-order gotcha: framebuf's RGB565 storage is little-endian, but the panel
expects big-endian pixels (st7789py's _ENCODE_PIXEL = ">H", needs_swap=False
on this build -- see display.py) -- every color must be pre-byte-swapped
(swap16()) before reaching fb.pixel()/fb.fill_rect(), or R/B come out
swapped.

180-degree prism-viewing offset: applied per-point via display.to_physical()
(inlined in _render_points() below for speed), NOT baked into the rotation
math, so the camera angle always means the same physical thing regardless of
panel mounting.

Q8 fixed-point + viper (see _render_points()): this MicroPython build's
viper can't truncate float->int ("can't convert float to int") and has no
viper `float` type at all, so the per-point rotate/project loop runs in Q8
fixed-point integers instead of floats -- ~30x faster than the
@micropython.native float version it replaced (~124ms/frame -> ~4ms/frame at
3000 points), reading/writing xs/ys/zs/colors/buf via ptr32/ptr16 casts
(bypasses fb.pixel()'s per-point Python call entirely).

Nudge-to-switch-orbital (see nudge.py): a detected L/R/U/D nudge advances
through cloud_common.ORBITAL_PRESETS. IMU init is try/except-wrapped -- a
board with no QMI8658 wired still runs the animation, just without nudge
control.

Point turnover / "buzz" / random zoom excursions: see CULL_FRACTION,
BUZZ_FRACTION, and ZOOM_EXCURSION_* below for what each does and why.
"""

import array
import math
import random
import time

import framebuf

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
FX_SCALE = 1 << FX_BITS  # Q8 fixed-point scale factor, see module docstring

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
                           # see module docstring's "why three axes, not two". Also kept close
                           # to ANGLE_STEP for the same "don't lag behind yaw" reason.
_TILT_ANGLE_START = 0.9   # tilt_angle/roll_angle start away from the degenerate all-zero pose
_ROLL_ANGLE_START = 2.1   # (where yaw alone can't move axis-aligned lobes at all), so even
                           # the first frame after boot isn't axis-locked

# Fly-over (see _fly_over()): camera starts at base_scale * factor and eases
# to base_scale over `frames` frames. Boot intro is slower/more dramatic
# than a preset switch, which is more dramatic than a mid-scene zoom
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
FPS_UPDATE_INTERVAL = 50
FPS_TEXT_POS = (2, 2)
TITLE_TEXT_POS = (2, 12)
LOADING_TEXT = "Loading..."
LOADING_TEXT_POS = (2, 22)


def swap16(color565):
    return ((color565 & 0xFF) << 8) | (color565 >> 8)


def _encode_color(level, sign):
    # r, g, b = ... then color565(r, g, b), NOT color565(*level_to_rgb(level, sign)) --
    # star-unpacking measured ~4x slower here (matters: called once per point).
    r, g, b = cloud_common.level_to_rgb(level, sign)
    return swap16(st7789.color565(r, g, b))


def _to_fixed(values):
    out = array.array('i', bytes(4 * len(values)))
    for i in range(len(values)):
        out[i] = int(values[i] * FX_SCALE)
    return out


class PresetState:
    """Everything one loaded orbital preset needs to render and turn over:
    float coordinates (kept alive for resample()), their Q8 fixed-point
    counterparts (what _render_points() actually reads), encoded colors,
    and the cloud_common.ResampleState to keep resampling from the same
    distribution. Bundled here instead of threaded through run() as a
    handful of loose variables.
    """

    def __init__(self, index):
        n, ell, m, label = cloud_common.ORBITAL_PRESETS[index]
        print("orbital: loading preset %d (%s, n=%d l=%d m=%d)..." % (index, label, n, ell, m))
        t0 = time.ticks_ms()

        xs, ys, zs, psi2, signs, sampler, rng, radial_coeff, legendre_coeff = cloud_common.build_point_cloud(n, ell, m)
        levels, psi2_sorted = cloud_common.compute_levels(psi2)

        self.xs, self.ys, self.zs = xs, ys, zs
        self.xs_fx = _to_fixed(xs)
        self.ys_fx = _to_fixed(ys)
        self.zs_fx = _to_fixed(zs)
        self.colors = array.array('H', bytes(2 * len(levels)))
        for i in range(len(levels)):
            self.colors[i] = _encode_color(levels[i], signs[i])

        self.title = cloud_common.title_for_preset(cloud_common.ORBITAL_PRESETS[index])
        self.base_scale, self.zoom_amplitude, _r_ref = cloud_common.scale_from_radii(xs, ys, zs)
        self.resample_state = cloud_common.ResampleState(
            sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted)

        print("orbital: %s loaded in %dms, scale=%.1f" % (
            label, time.ticks_diff(time.ticks_ms(), t0), self.base_scale))

    def resample(self, count):
        """Point turnover (see CULL_FRACTION/CULL_REFRESH_FRAMES): redraw
        `count` points from the same distribution, in place.
        """
        for idx, level, sign in cloud_common.resample_levels(self.resample_state, self.xs, self.ys, self.zs, count):
            if level > cloud_common.COLOR_MAX_LEVEL:
                level = cloud_common.COLOR_MAX_LEVEL  # see resample_levels()'s docstring
            self.xs_fx[idx] = int(self.xs[idx] * FX_SCALE)
            self.ys_fx[idx] = int(self.ys[idx] * FX_SCALE)
            self.zs_fx[idx] = int(self.zs[idx] * FX_SCALE)
            self.colors[idx] = _encode_color(level, sign)


@micropython.viper
def _render_points(buf, xs, ys, zs, colors, n: int,
                    cos_y_fx: int, sin_y_fx: int, cos_x_fx: int, sin_x_fx: int,
                    cos_z_fx: int, sin_z_fx: int, scale_fx: int,
                    cx: int, cy: int, w: int, h: int, frame_salt: int, buzz_threshold: int):
    """Rotate (yaw about Y, tilt about X, roll about Z -- all three needed,
    see module docstring), project, and draw every point directly into
    `buf` -- Q8 fixed-point only (see module docstring: viper can't do
    float->int here). Shift amounts (8, 16) are literal ints, not the
    FX_BITS constant -- referencing a module global from inside viper yields
    a boxed 'object', which can't mix with viper's native int arithmetic;
    keep in sync with FX_BITS by hand if it ever changes.

    Depth after yaw (rz1) is computed only to feed the tilt step, then
    dropped -- rendering stays depth-sort-free (see CLAUDE.md section 5), so
    the post-tilt/post-roll depth is never needed either.

    Every `>> N` here is preceded by `+ (1 << (N-1))` (128 for the >>8 steps,
    32768 for the final >>16): plain `>>` on a signed int is a floor (rounds
    toward -inf), not round-to-nearest, so without the offset every stage
    would be systematically biased low. The float-side equivalent of this
    (`int()` truncating toward zero in pc/orbital_view_pc.py's render_frame)
    was measured to bias points about 6-15% toward the screen's horizontal/
    vertical centerlines vs. the diagonals -- fixed there with round();
    this is the fixed-point way to remove the same class of bias.

    "Buzz" (see BUZZ_FRACTION in cloud_common.py): hv is a cheap
    multiplicative hash (668265261/374761393 -- Bob Jenkins'/xxHash's
    32-bit constants, picked over the more common 2654435761 because that
    one is >= 2^31 and doesn't fit viper's native int literal) of the point
    index and frame_salt, taking the high 16 bits (low bits of a
    multiplicative hash distribute poorly). buzz_threshold=0 disables it.
    Named `hv` not `h` -- `h` is the height parameter used just below.
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


def _render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, frame_salt=0):
    """Clear, draw the proton marker (via framebuf -- cheap, not
    once-per-point), then every point in `preset` at `angle`/`tilt_angle`/
    `roll_angle`/`scale`. Shared by _fly_over() and run()'s steady-state loop.
    """
    w1 = WIDTH - 1
    h1 = HEIGHT - 1

    fb.fill(0)
    proton_x = CENTER - PROTON_SIZE // 2
    proton_y = CENTER - PROTON_SIZE // 2
    fb.fill_rect(w1 - proton_x, h1 - proton_y, PROTON_SIZE, PROTON_SIZE, proton_color)

    cos_y_fx = int(math.cos(angle) * FX_SCALE)
    sin_y_fx = int(math.sin(angle) * FX_SCALE)
    cos_x_fx = int(math.cos(tilt_angle) * FX_SCALE)
    sin_x_fx = int(math.sin(tilt_angle) * FX_SCALE)
    cos_z_fx = int(math.cos(roll_angle) * FX_SCALE)
    sin_z_fx = int(math.sin(roll_angle) * FX_SCALE)
    scale_fx = int(scale * FX_SCALE)
    buzz_threshold = int(cloud_common.BUZZ_FRACTION * 65536)
    _render_points(buf, preset.xs_fx, preset.ys_fx, preset.zs_fx, preset.colors, len(preset.xs_fx),
                    cos_y_fx, sin_y_fx, cos_x_fx, sin_x_fx, cos_z_fx, sin_z_fx, scale_fx,
                    CENTER, CENTER, WIDTH, HEIGHT, frame_salt, buzz_threshold)


def _fly_over(d, fb, buf, preset, proton_color, text_color, angle, tilt_angle, roll_angle,
              start_scale, end_scale, frames):
    """Ease the projection scale from start_scale to end_scale over `frames`
    frames, rendering+blitting each one. Shared by the boot intro,
    nudge-triggered switches, and random zoom excursions. Returns the
    running (angle, tilt_angle, roll_angle) so rotation continues smoothly
    afterward.
    """
    two_pi = 2 * math.pi
    for i in range(frames):
        t = i / (frames - 1) if frames > 1 else 1.0
        scale = start_scale + (end_scale - start_scale) * t
        _render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, i)
        fb.text(preset.title, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
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


def _init_nudge_detector():
    """Best-effort IMU + nudge detector setup; None (with a printed warning)
    if the QMI8658 isn't present/answering -- an optional feature failing
    shouldn't take the animation down with it.
    """
    if qmi8658 is None or nudge is None:
        print("nudge: qmi8658/nudge modules not found, orbital switching via nudge disabled")
        return None
    try:
        imu = qmi8658.QMI8658()
        detector = nudge.NudgeDetector(imu)
        print("nudge: QMI8658 ready, nudge-controlled orbital switching enabled")
        return detector
    except OSError as e:
        print("nudge: IMU init failed (%s), orbital switching via nudge disabled" % e)
        return None


def _next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)


# Direction -> preset index step. R/U advance, L/D go back -- simplest
# mapping satisfying "a nudge in any of 4 directions changes the orbital",
# and nudge.py's axis calibration is still a placeholder (see that module),
# so a more elaborate per-direction mapping isn't worth it yet.
_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


def run():
    print("orbital: display init...")
    d = display_mod.init()
    print("orbital: display ready, %d presets available" % len(cloud_common.ORBITAL_PRESETS))

    preset_index = cloud_common.DEFAULT_PRESET_INDEX
    preset = PresetState(preset_index)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = swap16(st7789.color565(255, 0, 0))
    text_color = swap16(st7789.color565(255, 255, 255))

    detector = _init_nudge_detector()

    angle = 0.0
    tilt_angle = _TILT_ANGLE_START
    roll_angle = _ROLL_ANGLE_START
    zoom_angle = 0.0
    two_pi = 2 * math.pi

    angle, tilt_angle, roll_angle = _fly_over(
        d, fb, buf, preset, proton_color, text_color, angle, tilt_angle, roll_angle,
        preset.base_scale * INTRO_START_SCALE_FACTOR, preset.base_scale, INTRO_FRAMES)

    fps_text = "FPS: --"
    frame_count = 0
    fps_window_start = time.ticks_ms()

    cull_count = max(1, int(len(preset.xs) * cloud_common.CULL_FRACTION))
    cull_frame_count = 0

    # "Buzz" salt: just needs to change every frame (see _render_points()).
    buzz_frame = 0

    zoom_excursion_countdown = _next_zoom_excursion_countdown()

    while True:
        # Nudge check: switches presets and re-does the fly-over on a
        # detected L/R/U/D. LOADING_TEXT covers PresetState()'s ~1.5-2.5s
        # rebuild so the display doesn't just freeze on the old cloud.
        if detector is not None:
            raw = detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                step = _NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    preset_index = (preset_index + step) % len(cloud_common.ORBITAL_PRESETS)
                    fb.fill(0)
                    fb.text(LOADING_TEXT, LOADING_TEXT_POS[0], LOADING_TEXT_POS[1], text_color)
                    d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
                    preset = PresetState(preset_index)
                    cull_count = max(1, int(len(preset.xs) * cloud_common.CULL_FRACTION))
                    cull_frame_count = 0
                    angle, tilt_angle, roll_angle = _fly_over(
                        d, fb, buf, preset, proton_color, text_color, angle, tilt_angle, roll_angle,
                        preset.base_scale * SWITCH_START_SCALE_FACTOR, preset.base_scale,
                        SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion: pause breathing, fly to a random scale and
        # back (see ZOOM_EXCURSION_*). zoom_angle resets to 0 after --
        # sin(0) == 0 lines up exactly with where the excursion left off.
        # `continue`: this iteration's render already happened inside
        # _fly_over(), so skip the normal render/turnover/FPS bookkeeping.
        zoom_excursion_countdown -= 1
        if zoom_excursion_countdown <= 0:
            current_scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
            target_scale = preset.base_scale * random.uniform(ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                                ZOOM_EXCURSION_SCALE_MAX_FACTOR)
            angle, tilt_angle, roll_angle = _fly_over(
                d, fb, buf, preset, proton_color, text_color, angle, tilt_angle, roll_angle,
                current_scale, target_scale, ZOOM_EXCURSION_EASE_FRAMES)
            angle, tilt_angle, roll_angle = _fly_over(
                d, fb, buf, preset, proton_color, text_color, angle, tilt_angle, roll_angle,
                target_scale, preset.base_scale, ZOOM_EXCURSION_EASE_FRAMES)
            zoom_angle = 0.0
            zoom_excursion_countdown = _next_zoom_excursion_countdown()
            continue

        cull_frame_count += 1
        if cull_frame_count >= cloud_common.CULL_REFRESH_FRAMES:
            preset.resample(cull_count)
            cull_frame_count = 0

        scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
        _render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, buzz_frame)
        buzz_frame = buzz_frame + 1 if buzz_frame < 1_000_000 else 0
        fb.text(preset.title, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        fb.text(fps_text, FPS_TEXT_POS[0], FPS_TEXT_POS[1], text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        frame_count += 1
        if frame_count >= FPS_UPDATE_INTERVAL:
            now = time.ticks_ms()
            elapsed_ms = time.ticks_diff(now, fps_window_start)
            fps = 1000.0 * frame_count / elapsed_ms if elapsed_ms > 0 else 0.0
            fps_text = "FPS: %.1f" % fps
            print("orbital: %s" % fps_text)
            frame_count = 0
            fps_window_start = now

        angle += ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi
        zoom_angle += ZOOM_ANGLE_STEP
        if zoom_angle >= two_pi:
            zoom_angle -= two_pi
        time.sleep_ms(FRAME_DELAY_MS)


if __name__ == '__main__':
    run()
