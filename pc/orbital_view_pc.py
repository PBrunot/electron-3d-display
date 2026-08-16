"""PC debug port of micropython/orbital_view.py: same hydrogen-orbital
point-cloud animation, same nudge-to-switch-orbital control, same
point-turnover/"buzz" liveliness effects, running in a tkinter window
instead of on the ESP32-S3 panel -- see pc/README.md for how to run it.

Shared with the device via micropython/cloud_common.py (orbital math,
sampling, ranking, point-turnover -- see that module's docstring) and
micropython/nudge.py (gesture detection, imported unmodified; only the
"sensor" underneath it differs, see keyboard_imu.py). Nothing else is
duplicated -- what's left here is genuinely PC-only:

- Rendering: bytearray RGB buffer -> PIL Image -> tkinter Canvas, plain
  floats throughout (no Q8 fixed-point/viper -- that's an ESP32-viper
  workaround, see orbital_view.py's docstring; a desktop CPU doesn't need
  it). No 180-degree prism offset either -- there's no physical prism here.
- "Buzz" uses real per-point randomness instead of orbital_view.py's
  integer hash (that hash exists only because viper has no RNG/floats).
- Nudge input is the keyboard (arrow keys) via keyboard_imu.KeyboardIMU.
- N_POINTS=10000 (vs. the device's 3000) -- no render-loop budget to
  respect on a desktop CPU.
- Bounding sphere + rotating "H" marker: a debug aid with no device
  equivalent, since several presets look close to rotationally symmetric
  in plain orthographic projection and the rotation is hard to perceive
  from the point cloud alone.
"""

import math
import os
import random
import sys
import time

import micropython_shim  # noqa: F401 -- import for its side effect (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import cloud_common
import nudge

from keyboard_imu import KeyboardIMU

from PIL import Image, ImageDraw, ImageFont, ImageTk
import tkinter as tk

WIDTH = 480
HEIGHT = 480
CENTER = WIDTH // 2

DISPLAY_SCALE = 2  # tkinter window is WIDTH*DISPLAY_SCALE square; math stays at WIDTH/HEIGHT
DISPLAY_SIZE = (WIDTH * DISPLAY_SCALE, HEIGHT * DISPLAY_SCALE)

N_POINTS = 20000  # higher than the device's 3000 -- a desktop CPU has the headroom for it

# Bounding sphere + rotation marker (PC-only, see module docstring). The
# circle sits at radius r_ref (same p90 radius base_scale is calibrated
# against) and never rotates (a sphere's silhouette is a circle from every
# angle) -- a pure size anchor. The marker is a single reference vector,
# elevated near the pole (MARKER_ELEVATION_DEG) rather than lying in the
# horizontal plane -- 90deg would sit exactly on the Y rotation axis and
# never move; 0deg would sweep the equator (tried first, replaced on
# request: "on top of the sphere", plus it visually competed with the
# title text at the top of the frame). It's what visibly moves each frame,
# giving an unambiguous read on rotation direction/speed.
BOUNDING_SPHERE_COLOR = (70, 70, 90)
MARKER_TEXT = "H"
MARKER_FONT_SIZE = 15
MARKER_ELEVATION_DEG = 50.0
_MARKER_ELEVATION_RAD = math.radians(MARKER_ELEVATION_DEG)
MARKER_COLOR_BEHIND = (110, 110, 110)  # flat gray when rotating away from the viewer
MARKER_COLOR_FRONT = (255, 220, 40)    # vivid warm yellow when rotating toward the viewer --
                                        # a color shift (not just dimmer/brighter gray-blue)
                                        # reads as a much stronger front/back cue
_MARKER_FONT = ImageFont.load_default(size=MARKER_FONT_SIZE)  # loaded once, not per frame

ANGLE_STEP = 0.030
ZOOM_ANGLE_STEP = 0.016
TILT_ANGLE_STEP = 0.023   # second (X-axis) rotation's angular speed. Kept close to ANGLE_STEP
                           # (not much slower) on purpose: with tilt=roll=0, a point's screen-Y
                           # depends only on tilt+roll, NOT on yaw at all -- so if tilt/roll lag
                           # far behind yaw, axis-aligned lobes (e.g. 3d_x2-y2's) sit still for
                           # the first second or two while yaw visibly spins everything else,
                           # reading as "a fixed axis that doesn't rotate" even though it does
                           # eventually. Non-resonant vs. ANGLE_STEP/ROLL_ANGLE_STEP so the
                           # tumble doesn't fall into a short repeating loop.
ROLL_ANGLE_STEP = 0.017   # third (Z-axis) rotation's angular speed -- required, not cosmetic:
                           # yaw+tilt alone leave a point's screen-X depending only on its own
                           # (x,z), never on tilt, so anything near the world Y axis stays pinned
                           # to the vertical screen centerline without this third axis -- see
                           # orbital_view.py's module docstring for the full derivation. Also
                           # kept close to ANGLE_STEP for the same "don't lag behind yaw" reason.
_TILT_ANGLE_START = 0.9   # tilt_angle/roll_angle start away from the degenerate all-zero pose
_ROLL_ANGLE_START = 2.1   # (where yaw alone can't move axis-aligned lobes at all), so even
                           # frame 0 right after boot isn't axis-locked
FRAME_DELAY_MS = 20  # tkinter .after() delay; PC is fast enough this is the real throttle

# Debug isolation switches (temporary -- for confirming the yaw/tilt/roll
# rotation math in isolation, with no point-turnover or per-frame flicker
# muddying the picture). Flip back to False once done.
DEBUG_DISABLE_CULL = False   # point-turnover (resample) off: the cloud stays exactly as sampled
DEBUG_DISABLE_BUZZ = True   # per-frame flicker off: every point renders every frame

INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 18

# Random zoom excursions (ported from orbital_view.py's device loop -- see
# that module for the original): at randomized intervals, dive from the
# current breathing scale to a randomized target and back, layered on top
# of the constant sine-wave breathing so the motion doesn't read as purely
# mechanical. MAX_FACTOR=9.0 (device uses 5.0) -- PC has no render-loop
# budget to protect, so the dive can go deep enough to feel like passing
# through individual points into the electron cloud ("go deeper into the
# atom"), not just a bigger breath.
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 150
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 400
ZOOM_EXCURSION_SCALE_MIN_FACTOR = 0.35
ZOOM_EXCURSION_SCALE_MAX_FACTOR = 9.0
ZOOM_EXCURSION_EASE_FRAMES = 30

PROTON_SIZE = 3
PROTON_COLOR = (255, 0, 0)

# Per-point alpha blend for every sampled electron point (the nucleus marker
# above is NOT affected -- it represents one literal particle, not a
# probability cloud, so it stays fully opaque). Each point blends toward its
# own color by this fraction instead of overwriting the pixel outright:
# new = old + (color - old) * ELECTRON_ALPHA. A single isolated point then
# renders dimmer than its "true" color (blended toward the black
# background), while a pixel several points project onto in the same frame
# (common at these projection densities -- 240x240 screen space is coarse
# next to N_POINTS=3000-20000 samples) converges toward full brightness as
# each subsequent point blends further in -- i.e. apparent brightness starts
# tracking local sample DENSITY at a pixel, not just whether it's occupied,
# the way a translucent point cloud reads. 1.0 = opaque (old behavior,
# equivalent to the direct overwrite this replaces).
ELECTRON_ALPHA = 0.5

# Phosphor-style persistence (PC-only cosmetic; the device stays a hard
# clear+redraw -- see orbital_view.py, no budget on-device for this).
# Each frame fades the previous buffer toward black instead of clearing it,
# so points leave a trailing glow as they tumble -- softens the flicker from
# "buzz" turnover too, since a skipped point fades out instead of vanishing.
# Applied via bytes.translate() (one C-level lookup pass over the whole
# buffer) rather than a per-byte Python loop -- translate() is effectively
# free at 240x240x3 bytes/frame; a Python loop over the same bytes would not be.
ENABLE_PERSISTENCE = True
PERSISTENCE_DECAY = 100  # /256 kept per frame (~0.39) -- lower = shorter trails, 256 = never fades
_PERSISTENCE_TABLE = bytes((i * PERSISTENCE_DECAY) // 256 for i in range(256))

_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


def _next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)


class Preset:
    """PC equivalent of orbital_view.py's PresetState -- everything one
    loaded orbital needs to render and turn over. No Q8 fixed-point (see
    module docstring); colors are a plain list of (r, g, b) tuples.
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
    """Clear (or fade, see ENABLE_PERSISTENCE) `buf` (WIDTH*HEIGHT*3 RGB
    bytearray), draw the proton marker, then rotate (yaw about Y by `angle`,
    tilt about X by `tilt_angle`, roll about Z by `roll_angle` -- all three
    needed, see orbital_view.py's module docstring) /project/draw every
    point in `preset`. Each point is alpha-blended into the buffer, not
    overwritten -- see ELECTRON_ALPHA's module comment. Plain float math,
    direct buffer writes -- see module docstring for why no fixed-point is
    needed here.
    """
    if ENABLE_PERSISTENCE:
        buf[:] = buf.translate(_PERSISTENCE_TABLE)  # fade previous frame instead of clearing
    else:
        buf[:] = bytes(len(buf))  # fast bulk clear

    proton_x0 = CENTER - PROTON_SIZE // 2
    proton_y0 = CENTER - PROTON_SIZE // 2
    pr, pg, pb = PROTON_COLOR
    for py in range(proton_y0, proton_y0 + PROTON_SIZE):
        if 0 <= py < HEIGHT:
            for px in range(proton_x0, proton_x0 + PROTON_SIZE):
                if 0 <= px < WIDTH:
                    idx = (py * WIDTH + px) * 3
                    buf[idx], buf[idx + 1], buf[idx + 2] = pr, pg, pb

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

        rx1 = xs[i] * cos_yaw + zs[i] * sin_yaw
        rz1 = zs[i] * cos_yaw - xs[i] * sin_yaw
        ry2 = ys[i] * cos_tilt - rz1 * sin_tilt
        rx3 = rx1 * cos_roll - ry2 * sin_roll
        ry3 = rx1 * sin_roll + ry2 * cos_roll
        # round(), not int(): int() truncates toward zero, which is a biased
        # rounding rule (both +0.9 and -0.9 truncate to 0) -- disabling that
        # quantization bias to check whether it's contributing to the
        # axis-aligned density seen at the pixel-grid level.
        px = CENTER + round(rx3 * scale)
        py = CENTER - round(ry3 * scale)

        if 0 <= px < WIDTH and 0 <= py < HEIGHT:
            idx = (py * WIDTH + px) * 3
            cr, cg, cb = colors[i]
            buf[idx] = buf[idx] + int((cr - buf[idx]) * ELECTRON_ALPHA)
            buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * ELECTRON_ALPHA)
            buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * ELECTRON_ALPHA)


def draw_orbit_marker(draw, r_ref, scale, angle, tilt_angle, roll_angle, marker_text=MARKER_TEXT):
    """Bounding-sphere + rotating marker overlay -- see MARKER_TEXT's module
    comment for why this exists (a pure-orthographic cue for rotation on
    presets/clouds whose silhouette alone doesn't show it). Free function
    (not a method) so pc/atom_view_pc.py can reuse it unmodified with its
    own marker_text (the element symbol) instead of the hydrogen demo's
    fixed "H" -- an atom's spherically-averaged cloud (see atom_cloud.py)
    needs this cue even more, since its silhouette looks the same from
    every angle by construction.
    """
    px_r = r_ref * scale
    draw.ellipse((CENTER - px_r, CENTER - px_r, CENTER + px_r, CENTER + px_r),
                 outline=BOUNDING_SPHERE_COLOR)

    # Reference vector (horizontal_r, y0, 0) before rotation; rotated by the
    # same yaw+tilt+roll three-axis transform as every sampled point (see
    # render_frame()). Roll (about Z) never changes the Z coordinate, so
    # `rz` computed pre-roll is still the correct depth cue post-roll --
    # only rx/ry need the extra roll step.
    horizontal_r = r_ref * math.cos(_MARKER_ELEVATION_RAD)
    y0 = r_ref * math.sin(_MARKER_ELEVATION_RAD)
    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    rx1 = horizontal_r * cos_yaw
    rz1 = -horizontal_r * sin_yaw
    ry2 = y0 * cos_tilt - rz1 * sin_tilt
    rz = y0 * sin_tilt + rz1 * cos_tilt  # depth cue only -- render_frame()'s points don't need this
    rx3 = rx1 * cos_roll - ry2 * sin_roll
    ry3 = rx1 * sin_roll + ry2 * cos_roll
    marker_x = CENTER + rx3 * scale
    marker_y = CENTER - ry3 * scale

    # depth_frac: 0 rotating away from the viewer, 1 rotating toward -- the
    # only depth signal an orthographic projection can give. r_ref (not
    # horizontal_r) is the right normalizer now that yaw+tilt can swing rz
    # across the marker vector's full length, not just its horizontal
    # component (rotation preserves vector length, so |rz| never exceeds
    # r_ref).
    depth_frac = (rz / r_ref + 1.0) / 2.0 if r_ref > 1e-6 else 0.5
    marker_color = tuple(
        int(MARKER_COLOR_BEHIND[c] + depth_frac * (MARKER_COLOR_FRONT[c] - MARKER_COLOR_BEHIND[c]))
        for c in range(3))

    # Spoke from the nucleus to the marker -- same depth-interpolated color
    # as the marker text itself, so the whole radius (not just the letter)
    # reads as gray when rotating behind / lit up in front.
    draw.line((CENTER, CENTER, marker_x, marker_y), fill=marker_color)

    draw.text((marker_x, marker_y), marker_text, fill=marker_color, font=_MARKER_FONT, anchor='mm')


# Bottom-left physical-size reference bar (see draw_scale_bar() below).
# "Nice" round lengths only (1/2/5 x a power of ten -- the same ladder a
# ruler or a map's scale bar uses) so the printed number is always easy to
# read at a glance, never something like "37 px = 0.68374 units".
_SCALE_BAR_CANDIDATES = (
    0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
    1, 2, 5, 10, 20, 50, 100, 200, 500, 1000,
)
SCALE_BAR_MARGIN_X = 8
SCALE_BAR_MARGIN_Y = 8
SCALE_BAR_MAX_PX = 90
SCALE_BAR_TICK_PX = 4
SCALE_BAR_COLOR = (210, 210, 210)


def _pick_scale_bar_length(pixels_per_unit, max_bar_px):
    """Largest candidate from _SCALE_BAR_CANDIDATES whose on-screen length
    (candidate * pixels_per_unit) still fits under max_bar_px -- i.e. the
    most precise round number the bar can show without overflowing. Falls
    back to the smallest candidate if even that one would be too long
    (only possible at extreme zoom-in, where the bar is allowed to overflow
    max_bar_px rather than disappear to zero length).
    """
    best = _SCALE_BAR_CANDIDATES[0]
    for candidate in _SCALE_BAR_CANDIDATES:
        if candidate * pixels_per_unit <= max_bar_px:
            best = candidate
        else:
            break
    return best


def draw_scale_bar(draw, pixels_per_unit, unit_label, canvas_height=HEIGHT, max_bar_px=SCALE_BAR_MAX_PX):
    """Bottom-left physical-size reference bar, the way a microscope or map
    view shows one: a horizontal line `length` physical units long (a
    "nice" round number, see _pick_scale_bar_length()), labeled with that
    length and unit_label. Recomputed from the CURRENT pixels_per_unit
    every call (callers pass in the frame's live rendering scale, not a
    fixed one), so it tracks the camera's zoom-breathing/excursion dives
    instead of only being accurate at rest scale.

    pixels_per_unit <= 0 draws nothing (defensive only -- render_frame()'s
    scale is never <= 0 in normal operation).
    """
    if pixels_per_unit <= 0:
        return
    length = _pick_scale_bar_length(pixels_per_unit, max_bar_px)
    bar_px = length * pixels_per_unit

    x0 = SCALE_BAR_MARGIN_X
    y = canvas_height - SCALE_BAR_MARGIN_Y
    x1 = x0 + bar_px

    draw.line((x0, y, x1, y), fill=SCALE_BAR_COLOR)
    draw.line((x0, y - SCALE_BAR_TICK_PX, x0, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR)
    draw.line((x1, y - SCALE_BAR_TICK_PX, x1, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR)

    draw.text((x0, y - SCALE_BAR_TICK_PX - 12), "%g %s" % (length, unit_label), fill=SCALE_BAR_COLOR)


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

        self._fly_over(self.preset.base_scale * INTRO_START_SCALE_FACTOR, self.preset.base_scale, INTRO_FRAMES)
        self.root.after(0, self._tick)

    def run(self):
        self.root.mainloop()

    def _blit(self, scale, extra_text=None):
        image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(self.buf), 'raw', 'RGB', 0, 1)
        draw = ImageDraw.Draw(image)
        self._draw_bounding_sphere_and_marker(draw, scale)
        draw.text((2, 2), self.preset.title, fill=(255, 255, 255))
        if extra_text:
            draw.text((2, 12), extra_text, fill=(255, 255, 255))
        image = image.resize(DISPLAY_SIZE, Image.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.canvas.itemconfig(self.image_id, image=self.photo)

    def _draw_bounding_sphere_and_marker(self, draw, scale):
        """See draw_orbit_marker()'s docstring. Rotated by the same `angle`
        the cloud itself is rotated by, so it always matches the current
        frame.
        """
        draw_orbit_marker(draw, self.preset.r_ref, scale, self.angle, self.tilt_angle, self.roll_angle)

    def _fly_over(self, start_scale, end_scale, frames):
        """PC equivalent of orbital_view.py's _fly_over() -- blocking (uses
        self.root.update() per frame to keep the window responsive) since
        it's a short, one-shot camera move. Takes absolute scales (not a
        factor of base_scale) so it can ease to/from anywhere -- e.g. a
        zoom excursion's deep-dive target, not just back to base_scale.
        """
        for i in range(frames):
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = start_scale + (end_scale - start_scale) * t
            render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale)
            self._blit(scale)
            self.root.update()
            self.angle = (self.angle + ANGLE_STEP) % self.two_pi
            self.tilt_angle = (self.tilt_angle + TILT_ANGLE_STEP) % self.two_pi
            self.roll_angle = (self.roll_angle + ROLL_ANGLE_STEP) % self.two_pi

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
                    self._fly_over(self.preset.base_scale * SWITCH_START_SCALE_FACTOR, self.preset.base_scale,
                                    SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion: pause breathing, dive to a random scale
        # (deep in most of the time -- see ZOOM_EXCURSION_SCALE_MAX_FACTOR)
        # and back. zoom_angle resets to 0 after -- sin(0) == 0 lines up
        # exactly with where the excursion left off. Skips the normal
        # render/turnover below since _fly_over() already rendered+blitted
        # every frame of the dive.
        self.zoom_excursion_countdown -= 1
        if self.zoom_excursion_countdown <= 0:
            current_scale = self.preset.base_scale + self.preset.zoom_amplitude * math.sin(self.zoom_angle)
            target_scale = self.preset.base_scale * random.uniform(ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                                     ZOOM_EXCURSION_SCALE_MAX_FACTOR)
            self._fly_over(current_scale, target_scale, ZOOM_EXCURSION_EASE_FRAMES)
            self._fly_over(target_scale, self.preset.base_scale, ZOOM_EXCURSION_EASE_FRAMES)
            self.zoom_angle = 0.0
            self.zoom_excursion_countdown = _next_zoom_excursion_countdown()
            self.root.after(FRAME_DELAY_MS, self._tick)
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

        self.angle = (self.angle + ANGLE_STEP) % self.two_pi
        self.tilt_angle = (self.tilt_angle + TILT_ANGLE_STEP) % self.two_pi
        self.roll_angle = (self.roll_angle + ROLL_ANGLE_STEP) % self.two_pi
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run():
    OrbitalViewApp().run()


if __name__ == '__main__':
    run()
