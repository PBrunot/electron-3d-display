"""Browser port of pc/orbital_view_pc.py's hydrogen-orbital point-cloud
viewer, built on web_common.py's canvas/generator backend -- same pattern
web_atom.py uses for the atom viewer (see that module's docstring for why
the animation loop is generator-based, and why WebOrbitalApp below is a
"scene" class web_app.py owns rather than a standalone page driver).

Preset itself is unchanged in spirit from pc/orbital_view_pc.py's -- pure
cloud_common.py model calls, no PIL/tkinter dependency to begin with.
Point-turnover (resample()) is dropped: the PC/device viewers periodically
re-sample a fraction of points to keep the cloud "alive" even when the
camera stops moving, worthwhile there since a nudge-driven session can sit
on one orbital a long time; skipped here since a web demo visitor typically
cycles through orbitals quickly and it isn't worth another per-frame cost
in an interpreted browser loop.

Preset cycling is driven directly by request_step(+1/-1) instead of
replaying pc/orbital_view_pc.py's KeyboardIMU-simulated nudge gestures --
that indirection exists on PC/device to exercise nudge.py's real gesture
detector for parity with the physical board; a browser demo has no IMU to
simulate, so stepping the preset index directly is the simpler, equivalent
behavior for arrow-key/button input (see web_app.py's on_key()).
"""

import math
import time

import cloud_common

import web_common as wc
from web_common import (
    WIDTH, HEIGHT, CENTER, ZOOM_ANGLE_STEP,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    INTRO_FRAMES, INTRO_START_SCALE_FACTOR,
    SWITCH_TRANSITION_FRAMES, SWITCH_START_SCALE_FACTOR,
    TITLE_POS,
    render_frame, advance_rotation,
    fly_over_gen, zoom_excursion_gen, next_zoom_excursion_countdown,
    draw_orbit_marker_canvas, draw_scale_bar_canvas, draw_text_canvas,
)

N_POINTS = 6000  # trimmed from the PC viewer's 20000 -- see web_atom.py's
                  # N_POINTS comment; the interpreted per-point loop, not
                  # canvas resolution, is the real per-frame cost here


class Preset:
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

        print("orbital: %s loaded in %.2fs, scale=%.1f" % (label, time.time() - t0, self.base_scale))


class WebOrbitalApp:
    def __init__(self):
        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.preset_index = cloud_common.DEFAULT_PRESET_INDEX
        self.preset = Preset(self.preset_index)
        self.pending_step = None

        self.angle = 0.0
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi
        self.zoom_excursion_countdown = next_zoom_excursion_countdown()

        self.sequence = None  # active generator (intro/switch/excursion), if any

    def blit(self, scale):
        wc.blit_buf(self.buf)
        draw_orbit_marker_canvas(self.preset.r_ref, scale, self.angle, self.tilt_angle, self.roll_angle, 'H')
        draw_scale_bar_canvas(cloud_common, scale / cloud_common.PM_PER_BOHR, "pm")
        draw_text_canvas(TITLE_POS[0], TITLE_POS[1], self.preset.title, (255, 255, 255))

    def request_step(self, step):
        self.pending_step = step

    def start(self):
        """Kicks off the intro fly-over -- see web_atom.WebAtomApp.start()'s
        docstring for why this doesn't bind the canvas itself.
        """
        self.sequence = fly_over_gen(self, self.preset.base_scale * INTRO_START_SCALE_FACTOR,
                                      self.preset.base_scale, INTRO_FRAMES)

    def tick(self):
        if self.sequence is not None:
            try:
                next(self.sequence)
            except StopIteration:
                self.sequence = None
            return

        if self.pending_step is not None:
            step = self.pending_step
            self.pending_step = None
            self.preset_index = (self.preset_index + step) % len(cloud_common.ORBITAL_PRESETS)
            self.preset = Preset(self.preset_index)
            self.sequence = fly_over_gen(self, self.preset.base_scale * SWITCH_START_SCALE_FACTOR,
                                          self.preset.base_scale, SWITCH_TRANSITION_FRAMES)
            return

        self.zoom_excursion_countdown -= 1
        if self.zoom_excursion_countdown <= 0:
            # A single hydrogen orbital has no inner/outer shell split -- its
            # own r_ref serves as both bounds, shell_count stays 1 (the
            # default), matching orbital_view_pc.OrbitalViewApp._tick()'s
            # equivalent call.
            self.sequence = zoom_excursion_gen(self, self.preset.base_scale, self.preset.zoom_amplitude,
                                                self.preset.r_ref, self.preset.r_ref)
            return

        scale = self.preset.base_scale + self.preset.zoom_amplitude * math.sin(self.zoom_angle)
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale)
        self.blit(scale)

        advance_rotation(self)
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi
