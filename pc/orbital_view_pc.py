"""PC debug port of micropython/orbital_view.py: the same hydrogen-orbital
point-cloud animation and nudge-to-switch-orbital control, rendered in a
tkinter window instead of on the ESP32-S3 panel (see pc/README.md).

Everything shared with the device comes from micropython/cloud_common.py
(orbital math, sampling, ranking, point turnover) and micropython/nudge.py
(gesture detection, imported unmodified; only the "sensor" underneath it
differs -- keyboard_imu.KeyboardIMU). What's left here is genuinely PC-only:
plain-float rendering into a bytearray -> PIL Image -> tkinter Canvas (no Q8
fixed-point/viper -- that's an ESP32 workaround), real per-point randomness
for the "buzz" effect (viper has no RNG), a bounding-sphere + rotating "H"
marker so rotation is visible on nearly-symmetric presets, phosphor-style
persistence, and N_POINTS=20000 (no render-loop budget to respect).

The module-level helpers between the constants and the app classes are
shared with pc/atom_view_pc.py via import -- see the "Shared ... helpers"
section markers.
"""

import math
import os
import random
import sys
import time

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import cloud_common
import nudge

from keyboard_imu import KeyboardIMU

from PIL import Image, ImageDraw, ImageFont, ImageTk
import tkinter as tk

# --- Display geometry -------------------------------------------------------
WIDTH = 480
HEIGHT = 480
CENTER = WIDTH // 2
DISPLAY_SCALE = 2  # tkinter window is WIDTH*DISPLAY_SCALE square; math stays at WIDTH/HEIGHT
DISPLAY_SIZE = (WIDTH * DISPLAY_SCALE, HEIGHT * DISPLAY_SCALE)
N_POINTS = 20000  # more than the device's 3000 -- a desktop CPU has the headroom

# --- Camera motion ----------------------------------------------------------
# Yaw/tilt/roll angular speed per frame. Roll is required, not cosmetic:
# yaw+tilt alone leave a point's screen-X independent of tilt, pinning points
# near the world Y axis to the vertical screen centerline (see orbital_view.py's
# module docstring for the derivation). Tilt/roll are kept close to ANGLE_STEP
# (and non-resonant with it) so axis-aligned lobes visibly rotate instead of
# lagging behind yaw, and the tumble doesn't fall into a short repeating loop.
ANGLE_STEP = 0.030
TILT_ANGLE_STEP = 0.023
ROLL_ANGLE_STEP = 0.017
ZOOM_ANGLE_STEP = 0.016
# Tilt/roll start away from the degenerate all-zero pose (where yaw alone
# can't move axis-aligned lobes at all), so even frame 0 isn't axis-locked.
_TILT_ANGLE_START = 0.9
_ROLL_ANGLE_START = 2.1
FRAME_DELAY_MS = 20  # tkinter .after() delay; PC is fast enough this is the real throttle

# --- Intro / orbital-switch transitions ------------------------------------
INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 18

# --- Random zoom excursions -------------------------------------------------
# At randomized intervals, dive from the current breathing scale to a
# randomized target and back, layered on the constant sine breathing so the
# motion doesn't read as purely mechanical. Max factor is deeper than the
# device's 5.0 -- no render budget to protect, so a dive can feel like
# passing through the cloud itself.
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 150
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 400
ZOOM_EXCURSION_SCALE_MIN_FACTOR = 0.35
ZOOM_EXCURSION_SCALE_MAX_FACTOR = 9.0
ZOOM_EXCURSION_EASE_FRAMES = 30

# --- Bounding sphere + rotation marker (PC-only, see module docstring) ------
BOUNDING_SPHERE_COLOR = (70, 70, 90)
MARKER_TEXT = "H"
MARKER_FONT_SIZE = 15
# Elevated near the pole (not 0deg, which would sit exactly on the Y rotation
# axis and never move) so the marker visibly moves every frame, giving an
# unambiguous read on rotation direction/speed.
MARKER_ELEVATION_DEG = 50.0
_MARKER_ELEVATION_RAD = math.radians(MARKER_ELEVATION_DEG)
MARKER_COLOR_BEHIND = (110, 110, 110)  # rotating away from the viewer
MARKER_COLOR_FRONT = (255, 220, 40)    # rotating toward the viewer -- a warm
                                        # color shift reads much stronger than
                                        # a gray brightness change
_MARKER_FONT = ImageFont.load_default(size=MARKER_FONT_SIZE)  # loaded once, not per frame

# --- Nucleus ----------------------------------------------------------------
PROTON_SIZE = 3
PROTON_COLOR = (255, 0, 0)

# --- Electron point rendering ----------------------------------------------
# Each point alpha-blends toward its own color instead of overwriting the
# pixel (1.0 = opaque). Overlapping points converge toward full brightness,
# so apparent brightness tracks local sample DENSITY at a pixel -- the way a
# translucent point cloud reads. The nucleus above is NOT blended (one literal
# particle, not a probability cloud).
ELECTRON_ALPHA = 0.5

# Phosphor-style persistence (PC-only cosmetic; the device hard-clears each
# frame -- see orbital_view.py). Each frame fades the previous buffer toward
# black via bytes.translate() (one C-level pass, effectively free at
# 240x240x3 bytes/frame) instead of clearing, so points leave a trailing glow
# and skipped "buzz" points fade out instead of vanishing.
ENABLE_PERSISTENCE = True
PERSISTENCE_DECAY = 100  # /256 kept per frame (~0.39); lower = shorter trails, 256 = never fades
_PERSISTENCE_TABLE = bytes((i * PERSISTENCE_DECAY) // 256 for i in range(256))

# --- Scale bar (bottom-left physical-size reference) ------------------------
# "Nice" round lengths + the picking rule live in cloud_common.py
# (pick_scale_bar_length()), shared with the device renderer so a bar reads
# the same physical length on both. What's left here is PIL-specific geometry.
SCALE_BAR_MARGIN_X = 8
SCALE_BAR_MARGIN_Y = 8
SCALE_BAR_MAX_PX = 90
SCALE_BAR_TICK_PX = 4
SCALE_BAR_COLOR = (210, 210, 210)

# --- HUD text positions -----------------------------------------------------
TITLE_POS = (2, 2)
SUBTITLE_POS = (2, 12)

# --- Debug isolation switches -----------------------------------------------
# Set False to disable point-turnover (resample) or per-frame "buzz" flicker,
# to inspect the raw rotation math in isolation.
DEBUG_DISABLE_CULL = False
DEBUG_DISABLE_BUZZ = True

_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


def _next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)


# --- Shared render helpers (also used by pc/atom_view_pc.py) ----------------
def draw_nucleus(buf):
    """Draw the fully-opaque nucleus marker (small square) at screen center.
    The nucleus is one literal particle, not a probability cloud, so it never
    alpha-blends.
    """
    proton_x0 = CENTER - PROTON_SIZE // 2
    proton_y0 = CENTER - PROTON_SIZE // 2
    pr, pg, pb = PROTON_COLOR
    for py in range(proton_y0, proton_y0 + PROTON_SIZE):
        if 0 <= py < HEIGHT:
            for px in range(proton_x0, proton_x0 + PROTON_SIZE):
                if 0 <= px < WIDTH:
                    idx = (py * WIDTH + px) * 3
                    buf[idx], buf[idx + 1], buf[idx + 2] = pr, pg, pb


def rotate_yaw_tilt_roll(x, y, z, cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll):
    """Rotate (x, y, z) by yaw (about Y), tilt (about X), roll (about Z).
    Returns (rx, ry, rz). rz is the post-yaw-and-tilt depth -- roll (about Z)
    never changes z, so rz is the correct depth cue for clipping.
    """
    rx1 = x * cos_yaw + z * sin_yaw
    rz1 = z * cos_yaw - x * sin_yaw
    ry2 = y * cos_tilt - rz1 * sin_tilt
    rz = y * sin_tilt + rz1 * cos_tilt
    rx3 = rx1 * cos_roll - ry2 * sin_roll
    ry3 = rx1 * sin_roll + ry2 * cos_roll
    return rx3, ry3, rz


class Preset:
    """Everything one loaded orbital needs to render and turn over. PC
    equivalent of orbital_view.py's PresetState, minus Q8 fixed-point --
    colors are plain (r, g, b) tuples.
    """

    def __init__(self, index):
        n, ell, m, label = cloud_common.ORBITAL_PRESETS[index]
        print("orbital: loading preset %d (%s, n=%d l=%d m=%d)..." % (index, label, n, ell, m))
        t0 = time.time()

        xs, ys, zs, psi2, signs, sampler, rng, radial_coeff, legendre_coeff = cloud_common.build_point_cloud(
            n, ell, m, count=N_POINTS)
        levels, psi2_sorted = cloud_common.compute_levels(psi2)

        self.xs, self.ys, self.zs = xs, ys, zs
        self.colors = [cloud_common.level_to_rgb(level, sign) for level, sign in zip(levels, signs)]
        self.title = cloud_common.title_for_preset(cloud_common.ORBITAL_PRESETS[index])
        self.base_scale, self.zoom_amplitude, self.r_ref = cloud_common.scale_from_radii(xs, ys, zs)
        self.resample_state = cloud_common.ResampleState(
            sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted)

        print("orbital: %s loaded in %.2fs, scale=%.1f" % (label, time.time() - t0, self.base_scale))

    def resample(self, count):
        for idx, level, sign in cloud_common.resample_levels(self.resample_state, self.xs, self.ys, self.zs, count):
            if level > cloud_common.COLOR_MAX_LEVEL:
                level = cloud_common.COLOR_MAX_LEVEL  # see resample_levels()'s docstring
            self.colors[idx] = cloud_common.level_to_rgb(level, sign)


def render_frame(buf, preset, angle, tilt_angle, roll_angle, scale, buzz_fraction=0.0):
    """Clear (or fade, see ENABLE_PERSISTENCE) `buf`, draw the nucleus, then
    rotate (yaw/tilt/roll -- all three needed, see module docstring) and
    alpha-blend every point of `preset` into the buffer.
    """
    if ENABLE_PERSISTENCE:
        buf[:] = buf.translate(_PERSISTENCE_TABLE)  # fade previous frame instead of clearing
    else:
        buf[:] = bytes(len(buf))  # fast bulk clear

    draw_nucleus(buf)

    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    xs, ys, zs, colors = preset.xs, preset.ys, preset.zs, preset.colors
    for i in range(len(xs)):
        if buzz_fraction and random.random() < buzz_fraction:
            continue

        rx3, ry3, _rz = rotate_yaw_tilt_roll(xs[i], ys[i], zs[i],
                                             cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll)
        # round(), not int(): int() truncates toward zero, a biased rounding
        # rule that could contribute to axis-aligned density at pixel level.
        px = CENTER + round(rx3 * scale)
        py = CENTER - round(ry3 * scale)

        if 0 <= px < WIDTH and 0 <= py < HEIGHT:
            idx = (py * WIDTH + px) * 3
            cr, cg, cb = colors[i]
            buf[idx] = buf[idx] + int((cr - buf[idx]) * ELECTRON_ALPHA)
            buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * ELECTRON_ALPHA)
            buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * ELECTRON_ALPHA)


def draw_orbit_marker(draw, r_ref, scale, angle, tilt_angle, roll_angle, marker_text=MARKER_TEXT):
    """Bounding-sphere + rotating marker overlay -- a pure-orthographic
    rotation cue for presets whose silhouette alone doesn't show it (see
    MARKER_TEXT's comment). Free function so pc/atom_view_pc.py can reuse it
    unmodified with its own marker_text (the element symbol).
    """
    px_r = r_ref * scale
    draw.ellipse((CENTER - px_r, CENTER - px_r, CENTER + px_r, CENTER + px_r),
                 outline=BOUNDING_SPHERE_COLOR)

    # Reference vector (horizontal_r, y0, 0) rotated by the same yaw+tilt+roll
    # transform as every sampled point (see rotate_yaw_tilt_roll()).
    horizontal_r = r_ref * math.cos(_MARKER_ELEVATION_RAD)
    y0 = r_ref * math.sin(_MARKER_ELEVATION_RAD)
    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    rx3, ry3, rz = rotate_yaw_tilt_roll(horizontal_r, y0, 0.0,
                                        cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll)
    marker_x = CENTER + rx3 * scale
    marker_y = CENTER - ry3 * scale

    # depth_frac: 0 rotating away, 1 rotating toward -- the only depth signal
    # an orthographic projection gives. Rotation preserves vector length, so
    # |rz| never exceeds r_ref.
    depth_frac = (rz / r_ref + 1.0) / 2.0 if r_ref > 1e-6 else 0.5
    marker_color = tuple(
        int(MARKER_COLOR_BEHIND[c] + depth_frac * (MARKER_COLOR_FRONT[c] - MARKER_COLOR_BEHIND[c]))
        for c in range(3))

    # Spoke from the nucleus to the marker in the same depth-interpolated
    # color, so the whole radius reads gray-behind / lit-in-front.
    draw.line((CENTER, CENTER, marker_x, marker_y), fill=marker_color)

    draw.text((marker_x, marker_y), marker_text, fill=marker_color, font=_MARKER_FONT, anchor='mm')


def draw_scale_bar(draw, pixels_per_unit, unit_label, canvas_height=HEIGHT, max_bar_px=SCALE_BAR_MAX_PX):
    """Bottom-left physical-size reference bar, like a microscope/map scale:
    a horizontal line `length` physical units long (a "nice" round number,
    see cloud_common.pick_scale_bar_length()), labeled with that length and
    unit_label. Recomputed from the frame's live pixels_per_unit every call
    so it tracks the camera's zoom-breathing/excursion dives. pixels_per_unit
    <= 0 draws nothing (defensive).
    """
    if pixels_per_unit <= 0:
        return
    length, label = cloud_common.pick_scale_bar_length(pixels_per_unit, max_bar_px)
    bar_px = length * pixels_per_unit

    x0 = SCALE_BAR_MARGIN_X
    y = canvas_height - SCALE_BAR_MARGIN_Y
    x1 = x0 + bar_px

    draw.line((x0, y, x1, y), fill=SCALE_BAR_COLOR)
    draw.line((x0, y - SCALE_BAR_TICK_PX, x0, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR)
    draw.line((x1, y - SCALE_BAR_TICK_PX, x1, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR)

    draw.text((x0, y - SCALE_BAR_TICK_PX - 12), "%s %s" % (label, unit_label), fill=SCALE_BAR_COLOR)


# --- Shared app helpers (also used by pc/atom_view_pc.py) -------------------
def advance_rotation(app):
    """Advance yaw/tilt/roll by one normal-viewing step."""
    app.angle = (app.angle + ANGLE_STEP) % app.two_pi
    app.tilt_angle = (app.tilt_angle + TILT_ANGLE_STEP) % app.two_pi
    app.roll_angle = (app.roll_angle + ROLL_ANGLE_STEP) % app.two_pi


def fly_over(app, start_scale, end_scale, frames):
    """Short, one-shot camera move -- blocking (app.root.update() per frame
    keeps the window responsive). Takes absolute scales so it can ease
    to/from anywhere, not just back to base_scale.
    """
    for i in range(frames):
        t = i / (frames - 1) if frames > 1 else 1.0
        scale = start_scale + (end_scale - start_scale) * t
        render_frame(app.buf, app.preset, app.angle, app.tilt_angle, app.roll_angle, scale)
        app._blit(scale)
        app.root.update()
        advance_rotation(app)


def maybe_zoom_excursion(app, base_scale, zoom_amplitude):
    """If the excursion countdown expired, dive to a random scale and back
    and return True -- the caller should skip its normal frame, since the
    dive already blitted every frame of itself. `base_scale`/`zoom_amplitude`
    come from the caller so the atom viewer can dive relative to the user's
    manual zoom factor. zoom_angle resets to 0 after -- sin(0) == 0 lines up
    exactly with where the dive left off.
    """
    app.zoom_excursion_countdown -= 1
    if app.zoom_excursion_countdown > 0:
        return False
    current_scale = base_scale + zoom_amplitude * math.sin(app.zoom_angle)
    target_scale = base_scale * random.uniform(ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                ZOOM_EXCURSION_SCALE_MAX_FACTOR)
    fly_over(app, current_scale, target_scale, ZOOM_EXCURSION_EASE_FRAMES)
    fly_over(app, target_scale, base_scale, ZOOM_EXCURSION_EASE_FRAMES)
    app.zoom_angle = 0.0
    app.zoom_excursion_countdown = _next_zoom_excursion_countdown()
    app.root.after(FRAME_DELAY_MS, app._tick)
    return True


def blit_to_canvas(app, overlays):
    """Convert app.buf to a tkinter canvas image, letting `overlays(draw)`
    add PIL overlays (marker, scale bar, title) in between. Shared by both
    viewers' _blit methods.
    """
    image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(app.buf), 'raw', 'RGB', 0, 1)
    draw = ImageDraw.Draw(image)
    overlays(draw)
    image = image.resize(DISPLAY_SIZE, Image.NEAREST)
    app.photo = ImageTk.PhotoImage(image)
    app.canvas.itemconfig(app.image_id, image=app.photo)


class OrbitalViewApp:
    """tkinter app driving render_frame() -- the PC equivalent of
    orbital_view.py's run(), restructured around tkinter's non-blocking
    `.after()` scheduling instead of a blocking `while True` loop.
    """

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Orbital viewer -- PC debug (arrow keys = nudge, close window to quit)")

        self.canvas = tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                 bg='black', highlightthickness=0)
        self.canvas.pack()
        self.canvas.focus_set()

        tk.Label(self.root, text="Arrow keys = nudge (switch orbital). Close window to quit.",
                 fg='white', bg='black').pack(fill='x')

        self.imu = KeyboardIMU(self.canvas)
        self.detector = nudge.NudgeDetector(self.imu)

        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.photo = None  # kept alive on self; tkinter drops PhotoImages with no live reference
        self.image_id = self.canvas.create_image(0, 0, anchor='nw')

        self.preset_index = cloud_common.DEFAULT_PRESET_INDEX
        self.preset = Preset(self.preset_index)
        self.cull_count = max(1, int(len(self.preset.xs) * cloud_common.CULL_FRACTION))
        self.cull_frame_count = 0

        self.angle = 0.0
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi
        self.zoom_excursion_countdown = _next_zoom_excursion_countdown()

        fly_over(self, self.preset.base_scale * INTRO_START_SCALE_FACTOR, self.preset.base_scale, INTRO_FRAMES)
        self.root.after(0, self._tick)

    def run(self):
        self.root.mainloop()

    def _blit(self, scale, extra_text=None):
        def overlays(draw):
            self._draw_bounding_sphere_and_marker(draw, scale)
            # Scale (px per Bohr radius, THIS frame -- varies with zoom
            # breathing/excursions) -> px per picometer, so the bar always
            # reflects the camera's current zoom, not just the resting one.
            draw_scale_bar(draw, scale / cloud_common.PM_PER_BOHR, "pm")
            draw.text(TITLE_POS, self.preset.title, fill=(255, 255, 255))
            if extra_text:
                draw.text(SUBTITLE_POS, extra_text, fill=(255, 255, 255))
        blit_to_canvas(self, overlays)

    def _draw_bounding_sphere_and_marker(self, draw, scale):
        """See draw_orbit_marker()'s docstring; rotated by the same `angle`
        the cloud itself is rotated by, so it always matches the frame.
        """
        draw_orbit_marker(draw, self.preset.r_ref, scale, self.angle, self.tilt_angle, self.roll_angle)

    def _tick(self):
        if self.detector is not None:
            raw = self.detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = self.detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                step = _NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    self.preset_index = (self.preset_index + step) % len(cloud_common.ORBITAL_PRESETS)
                    self.preset = Preset(self.preset_index)
                    self.cull_count = max(1, int(len(self.preset.xs) * cloud_common.CULL_FRACTION))
                    self.cull_frame_count = 0
                    fly_over(self, self.preset.base_scale * SWITCH_START_SCALE_FACTOR, self.preset.base_scale,
                             SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion: skip the normal render/turnover below since
        # the dive already blitted every frame of itself (see maybe_zoom_excursion()).
        if maybe_zoom_excursion(self, self.preset.base_scale, self.preset.zoom_amplitude):
            return

        if not DEBUG_DISABLE_CULL:
            self.cull_frame_count += 1
            if self.cull_frame_count >= cloud_common.CULL_REFRESH_FRAMES:
                self.preset.resample(self.cull_count)
                self.cull_frame_count = 0

        scale = self.preset.base_scale + self.preset.zoom_amplitude * math.sin(self.zoom_angle)
        buzz_fraction = 0.0 if DEBUG_DISABLE_BUZZ else cloud_common.BUZZ_FRACTION
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale,
                     buzz_fraction=buzz_fraction)
        self._blit(scale)

        advance_rotation(self)
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run():
    OrbitalViewApp().run()


if __name__ == '__main__':
    run()
