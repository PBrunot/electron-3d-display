"""PC debug port of micropython/orbital_view.py: the same hydrogen-orbital
point-cloud animation and nudge-to-switch-orbital control, rendered in a
tkinter window instead of on the ESP32-S3 panel (see pc/README.md).

Everything shared with the device comes from micropython/cloud_common.py
(orbital math, sampling, ranking, point turnover) and micropython/nudge.py
(gesture detection, imported unmodified; only the "sensor" underneath it
differs -- keyboard_imu.KeyboardIMU). Render/camera plumbing shared with
pc/atom_view_pc.py (display geometry, tumble, transitions, nucleus/marker/
scale-bar/persistence) lives in pc/viewer_common.py. What's left here is
genuinely orbital-specific: Preset (point-turnover), N_POINTS=20000 (more
than the device's 3000 -- desktop CPU has the headroom), no Q8 fixed-point/
viper (that's an ESP32 workaround), real per-point randomness for the "buzz"
effect (viper has no RNG), and the nudge-driven app loop.
"""

import math
import os
import sys
import time

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import cloud_common
import nudge

from keyboard_imu import KeyboardIMU

from viewer_common import (
    WIDTH, HEIGHT, CENTER, DISPLAY_SIZE,
    ANGLE_STEP, TILT_ANGLE_STEP, ROLL_ANGLE_STEP, ZOOM_ANGLE_STEP,
    _TILT_ANGLE_START, _ROLL_ANGLE_START, FRAME_DELAY_MS,
    INTRO_START_SCALE_FACTOR, INTRO_FRAMES,
    SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES,
    TITLE_POS, SUBTITLE_POS,
    render_frame, draw_orbit_marker, draw_scale_bar,
    advance_rotation, fly_over, maybe_zoom_excursion, blit_to_canvas,
    _next_zoom_excursion_countdown,
)

import tkinter as tk

N_POINTS = 20000  # more than the device's 3000 -- a desktop CPU has the headroom

# --- Debug isolation switches -----------------------------------------------
# Set False to disable point-turnover (resample) or per-frame "buzz" flicker,
# to inspect the raw rotation math in isolation.
DEBUG_DISABLE_CULL = False
DEBUG_DISABLE_BUZZ = True

_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


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
