"""PC-only viewer for atom_cloud.py's multi-electron point clouds -- same
tumbling-camera rendering as orbital_view_pc.py's hydrogen-preset viewer
(reuses its render_frame()/draw_orbit_marker() unmodified), but cycling
through atomic number Z instead of (n, ell, m) presets.

No per-frame point turnover here (unlike orbital_view_pc.py's Preset,
AtomPreset.resample() is a no-op) -- atom_cloud.py's cloud is a mixture of
several subshells with different Z_eff/table each, and cloud_common.py's
resample_levels()/ResampleState only knows how to redraw from a SINGLE
orbital's distribution. Wiring per-subshell turnover through a mixture would
be straightforward but wasn't needed to validate the reuse question this
mode exists to answer, so it's left out of this first pass; the cloud is
static after each element loads.

    python3 pc/atom_main.py [Z]

Up/Down arrow keys change the atomic number Z live (same fly-over
transition as switching a hydrogen preset in orbital_view_pc.py). Close the
window to quit.
"""

import math
import os
import random
import sys
import time

import micropython_shim  # noqa: F401 -- import for its side effect (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import atom_cloud
import slater

from orbital_view_pc import (
    CENTER, DISPLAY_SIZE, HEIGHT, WIDTH,
    INTRO_FRAMES, INTRO_START_SCALE_FACTOR,
    SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES,
    FRAME_DELAY_MS, ANGLE_STEP, TILT_ANGLE_STEP, ROLL_ANGLE_STEP,
    ZOOM_ANGLE_STEP, ZOOM_EXCURSION_SCALE_MIN_FACTOR, ZOOM_EXCURSION_SCALE_MAX_FACTOR,
    ZOOM_EXCURSION_EASE_FRAMES,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    render_frame, draw_orbit_marker, draw_scale_bar, _next_zoom_excursion_countdown,
)

from PIL import Image, ImageDraw, ImageTk
import tkinter as tk

N_POINTS = 10000
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell


class AtomPreset:
    """PC equivalent of orbital_view_pc.Preset, for one atomic number Z
    instead of one (n, ell, m) orbital. Same public shape (xs/ys/zs/colors/
    title/base_scale/zoom_amplitude/r_ref/resample()) so render_frame() and
    the fly-over/zoom-breathing loop below need no changes to accept it.
    """

    def __init__(self, z):
        print("atom: loading Z=%d (%s)..." % (z, slater.element_symbol(z)))
        t0 = time.time()

        xs, ys, zs, colors, config = atom_cloud.build_atom_point_cloud(z, count=N_POINTS)

        self.xs, self.ys, self.zs, self.colors = xs, ys, zs, colors
        self.title = atom_cloud.title_for_atom(z, config)
        self.base_scale, self.zoom_amplitude, self.r_ref = atom_cloud.scale_for_atom(
            xs, ys, zs, atom_cloud.PIXELS_PER_BOHR)

        print("atom: %s loaded in %.2fs, scale=%.1f" % (
            slater.element_symbol(z), time.time() - t0, self.base_scale))

    def resample(self, count):
        pass  # static cloud -- see module docstring


class AtomViewApp:
    """tkinter app driving render_frame() over AtomPreset -- structurally a
    trimmed-down copy of orbital_view_pc.OrbitalViewApp with the nudge-based
    preset switch replaced by Up/Down changing Z, and no bounding-sphere-
    marker/persistence differences (those come straight from
    orbital_view_pc.py). Kept as its own small class rather than subclassing
    OrbitalViewApp -- the two differ in exactly the input-handling bit
    (nudge vs. arrow-key Z change), and forcing that through inheritance
    would need overriding most of _tick() anyway.
    """

    def __init__(self, z=DEFAULT_Z):
        self.root = tk.Tk()
        self.root.title("Atom viewer -- PC debug (Up/Down = change element, close window to quit)")

        self.canvas = tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                 bg='black', highlightthickness=0)
        self.canvas.pack()
        self.canvas.focus_set()

        tk.Label(self.root, text="Up/Down = change element (Z). Close window to quit.",
                 fg='white', bg='black').pack(fill='x')

        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.photo = None  # kept alive on self; tkinter drops PhotoImages with no live reference
        self.image_id = self.canvas.create_image(0, 0, anchor='nw')

        self.z = z
        self.preset = AtomPreset(self.z)
        self._pending_z = None

        self.canvas.bind('<Up>', lambda e: self._request_z(1))
        self.canvas.bind('<Down>', lambda e: self._request_z(-1))

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

    def _request_z(self, step):
        new_z = self.z + step
        if 1 <= new_z <= slater.MAX_Z:
            self._pending_z = new_z

    def _blit(self, scale):
        image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(self.buf), 'raw', 'RGB', 0, 1)
        draw = ImageDraw.Draw(image)
        draw_orbit_marker(draw, self.preset.r_ref, scale, self.angle, self.tilt_angle, self.roll_angle,
                           marker_text=slater.element_symbol(self.z))
        # scale (px per Bohr radius, THIS frame -- varies with zoom
        # breathing/excursions) -> px per Angstrom, so the bar always
        # reflects the camera's current zoom, not just the resting one.
        draw_scale_bar(draw, scale / atom_cloud.ANGSTROM_PER_BOHR, "Å")
        draw.text((2, 2), self.preset.title, fill=(255, 255, 255))
        image = image.resize(DISPLAY_SIZE, Image.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.canvas.itemconfig(self.image_id, image=self.photo)

    def _fly_over(self, start_scale, end_scale, frames):
        """See orbital_view_pc.OrbitalViewApp._fly_over() -- identical
        approach (blocking per-frame root.update() loop), duplicated rather
        than shared because it closes over self.buf/preset/angle state that
        differs by app instance, not by class.
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
        if self._pending_z is not None:
            self.z = self._pending_z
            self._pending_z = None
            self.preset = AtomPreset(self.z)
            self._fly_over(self.preset.base_scale * SWITCH_START_SCALE_FACTOR, self.preset.base_scale,
                            SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion -- see orbital_view_pc.OrbitalViewApp._tick()'s
        # matching block for the rationale; identical here.
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

        scale = self.preset.base_scale + self.preset.zoom_amplitude * math.sin(self.zoom_angle)
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale)
        self._blit(scale)

        self.angle = (self.angle + ANGLE_STEP) % self.two_pi
        self.tilt_angle = (self.tilt_angle + TILT_ANGLE_STEP) % self.two_pi
        self.roll_angle = (self.roll_angle + ROLL_ANGLE_STEP) % self.two_pi
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run(z=DEFAULT_Z):
    AtomViewApp(z).run()


if __name__ == '__main__':
    run(int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_Z)
