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

WIDTH = 240
HEIGHT = 240
CENTER = WIDTH // 2

DISPLAY_SCALE = 3  # tkinter window is WIDTH*DISPLAY_SCALE square; math stays at WIDTH/HEIGHT
DISPLAY_SIZE = (WIDTH * DISPLAY_SCALE, HEIGHT * DISPLAY_SCALE)

N_POINTS = 10000  # higher than the device's 3000 -- a desktop CPU has the headroom for it

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
MARKER_COLOR_DIM = 140     # brightness when the marker is rotating away from the viewer
MARKER_COLOR_BRIGHT = 255  # brightness when rotating toward the viewer
MARKER_BLUE = 40
_MARKER_FONT = ImageFont.load_default(size=MARKER_FONT_SIZE)  # loaded once, not per frame

ANGLE_STEP = 0.030
ZOOM_ANGLE_STEP = 0.016
FRAME_DELAY_MS = 20  # tkinter .after() delay; PC is fast enough this is the real throttle

INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 18

PROTON_SIZE = 3
PROTON_COLOR = (255, 0, 0)

_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


class Preset:
    """PC equivalent of orbital_view.py's PresetState -- everything one
    loaded orbital needs to render and turn over. No Q8 fixed-point (see
    module docstring); colors are a plain list of (r, g, b) tuples.
    """

    def __init__(self, index):
        n, ell, m, label = cloud_common.ORBITAL_PRESETS[index]
        print("orbital: loading preset %d (%s, n=%d l=%d m=%d)..." % (index, label, n, ell, m))
        t0 = time.time()

        xs, ys, zs, psi2, sampler, rng, radial_coeff, legendre_coeff = cloud_common.build_point_cloud(
            n, ell, m, count=N_POINTS)
        levels, psi2_sorted = cloud_common.compute_levels(psi2)

        self.xs, self.ys, self.zs = xs, ys, zs
        self.colors = [cloud_common.level_to_rgb(level) for level in levels]
        self.title = cloud_common.title_for_preset(cloud_common.ORBITAL_PRESETS[index])
        self.base_scale, self.zoom_amplitude, self.r_ref = cloud_common.scale_from_radii(xs, ys, zs)
        self.resample_state = cloud_common.ResampleState(
            sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted)

        print("orbital: %s loaded in %.2fs, scale=%.1f" % (label, time.time() - t0, self.base_scale))

    def resample(self, count):
        for idx, level in cloud_common.resample_levels(self.resample_state, self.xs, self.ys, self.zs, count):
            if level > cloud_common.COLOR_MAX_LEVEL:
                level = cloud_common.COLOR_MAX_LEVEL  # see resample_levels()'s docstring
            self.colors[idx] = cloud_common.level_to_rgb(level)


def render_frame(buf, preset, angle, scale, buzz_fraction=0.0):
    """Clear `buf` (WIDTH*HEIGHT*3 RGB bytearray), draw the proton marker,
    then rotate/project/draw every point in `preset`. Plain float math,
    direct buffer writes -- see module docstring for why no fixed-point is
    needed here.
    """
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

    c = math.cos(angle)
    s = math.sin(angle)
    xs, ys, zs, colors = preset.xs, preset.ys, preset.zs, preset.colors
    for i in range(len(xs)):
        if buzz_fraction and random.random() < buzz_fraction:
            continue

        rx = xs[i] * c + zs[i] * s
        sx = CENTER + int(rx * scale)
        sy = CENTER - int(ys[i] * scale)

        if 0 <= sx < WIDTH and 0 <= sy < HEIGHT:
            idx = (sy * WIDTH + sx) * 3
            buf[idx], buf[idx + 1], buf[idx + 2] = colors[i]


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
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi

        self._fly_over(INTRO_START_SCALE_FACTOR, INTRO_FRAMES)
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
        """See BOUNDING_SPHERE_COLOR's comment. Rotated by the same `angle`
        the cloud itself is rotated by, so it always matches the current
        frame.
        """
        r_ref = self.preset.r_ref
        px_r = r_ref * scale
        draw.ellipse((CENTER - px_r, CENTER - px_r, CENTER + px_r, CENTER + px_r),
                     outline=BOUNDING_SPHERE_COLOR)

        # Reference vector (horizontal_r, y0, 0) before rotation; only the
        # X/Z part rotates (same rx = x*c + z*s as every sampled point).
        horizontal_r = r_ref * math.cos(_MARKER_ELEVATION_RAD)
        y0 = r_ref * math.sin(_MARKER_ELEVATION_RAD)
        c = math.cos(self.angle)
        s = math.sin(self.angle)
        rx = horizontal_r * c
        rz = -horizontal_r * s  # depth cue only -- render_frame()'s points don't need this
        marker_x = CENTER + rx * scale
        marker_y = CENTER - y0 * scale

        # depth_frac: 0 rotating away from the viewer, 1 rotating toward --
        # the only depth signal an orthographic projection can give for
        # motion confined to a horizontal chord.
        depth_frac = (rz / horizontal_r + 1.0) / 2.0 if horizontal_r > 1e-6 else 0.5
        brightness = int(MARKER_COLOR_DIM + depth_frac * (MARKER_COLOR_BRIGHT - MARKER_COLOR_DIM))
        marker_color = (brightness, brightness, MARKER_BLUE)

        draw.text((marker_x, marker_y), MARKER_TEXT, fill=marker_color, font=_MARKER_FONT, anchor='mm')

    def _fly_over(self, start_factor, frames):
        """PC equivalent of orbital_view.py's _fly_over() -- blocking (uses
        self.root.update() per frame to keep the window responsive) since
        it's a short, one-shot camera move.
        """
        start_scale = self.preset.base_scale * start_factor
        end_scale = self.preset.base_scale
        for i in range(frames):
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = start_scale + (end_scale - start_scale) * t
            render_frame(self.buf, self.preset, self.angle, scale)
            self._blit(scale)
            self.root.update()
            self.angle = (self.angle + ANGLE_STEP) % self.two_pi

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
                    self._fly_over(SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES)

        self.cull_frame_count += 1
        if self.cull_frame_count >= cloud_common.CULL_REFRESH_FRAMES:
            self.preset.resample(self.cull_count)
            self.cull_frame_count = 0

        scale = self.preset.base_scale + self.preset.zoom_amplitude * math.sin(self.zoom_angle)
        render_frame(self.buf, self.preset, self.angle, scale, buzz_fraction=cloud_common.BUZZ_FRACTION)
        self._blit(scale)

        self.angle = (self.angle + ANGLE_STEP) % self.two_pi
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run():
    OrbitalViewApp().run()


if __name__ == '__main__':
    run()
