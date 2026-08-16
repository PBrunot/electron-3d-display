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
transition as switching a hydrogen preset in orbital_view_pc.py). D triggers
a one-shot shell-dissection sequence (see AtomViewApp._run_dissection()):
the cloud keeps tumbling throughout (same yaw/tilt/roll rotation as normal
viewing, never paused) while the near half (rotated z > 0, i.e. the half
currently facing the camera) is hidden each frame, revealing a rotating
cutaway -- since the clip is applied in camera space every frame, which
PHYSICAL points fall inside it changes continuously as the atom turns, the
way a peeled sphere would look mid-spin. The view then zooms shell by shell
from outermost to innermost, dimming every other shell to a flat gray and
showing the current one's electron count/subshell makeup as on-screen text;
the nucleus (small red square) plus a "Z=n" note stay visible the whole
sequence, in every shell. The currently-exploded shell's points switch to
phase coloring (cloud_common.PHASE_POSITIVE_RGB/PHASE_NEGATIVE_RGB, by sign
of the real wavefunction -- see atom_cloud.build_atom_point_cloud()'s
`signs`) wherever that's meaningful -- a shell isolated on its own would
otherwise render as one flat SHELL_RGB blob, telling you nothing about its
actual (often lobed) shape; a full/isotropic subshell within that same
shell has no sign to show and keeps reading as its normal shell color.
Finally the view zooms back out and un-cuts. Close the window to quit.
"""

import math
import os
import random
import sys
import time

import micropython_shim  # noqa: F401 -- import for its side effect (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import atom_cloud
import cloud_common
import slater

from orbital_view_pc import (
    CENTER, DISPLAY_SIZE, HEIGHT, WIDTH,
    INTRO_FRAMES, INTRO_START_SCALE_FACTOR,
    SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES,
    FRAME_DELAY_MS, ANGLE_STEP, TILT_ANGLE_STEP, ROLL_ANGLE_STEP,
    ZOOM_ANGLE_STEP, ZOOM_EXCURSION_SCALE_MIN_FACTOR, ZOOM_EXCURSION_SCALE_MAX_FACTOR,
    ZOOM_EXCURSION_EASE_FRAMES,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    PROTON_SIZE, PROTON_COLOR,
    render_frame, draw_orbit_marker, draw_scale_bar, _next_zoom_excursion_countdown,
)

from PIL import Image, ImageDraw, ImageTk
import tkinter as tk

N_POINTS = 10000
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell

# Manual zoom (mouse wheel / +- keys) -- a persistent multiplier on top of
# preset.base_scale, independent of the automatic zoom-breathing/excursion
# animation (see _effective_base_scale()/_effective_zoom_amplitude() below).
# Multiplicative step (not additive) so each notch/keypress feels like the
# same relative zoom whether already zoomed in or out.
ZOOM_FACTOR_MIN = 0.15
ZOOM_FACTOR_MAX = 8.0
ZOOM_FACTOR_STEP = 1.1

# Shell-dissection sequence (see module docstring / AtomViewApp._run_dissection()).
DISSECT_TARGET_PX = 100.0  # on-screen p90 radius each shell's disc is zoomed to fill
DISSECT_SHADE_GRAY = (70, 70, 70)  # flat gray every non-active shell's points are drawn in
DISSECT_CLIP_OPEN = 0.0     # clip threshold that hides rotated-z > 0 (the "cut" is open)
DISSECT_CLIP_CLOSED = 1.0e6  # clip threshold no real point can exceed (nothing hidden -- "cut" is shut)
DISSECT_ORIENT_FRAMES = 40   # ease the clip open while the cloud keeps tumbling
DISSECT_ZOOM_FRAMES = 40     # ease camera scale from one shell to the next
DISSECT_HOLD_SECONDS = 2   # real-time pause on each shell with its label shown, still tumbling
DISSECT_CLOSE_FRAMES = 80    # ease the cut shut on the way back to the resting scale
DISSECT_FRAME_DELAY_S = FRAME_DELAY_MS / 1000.0  # paces the dissection's blocking loops to the
                                                   # same rotation speed normal viewing uses --
                                                   # unlike _fly_over()'s transitions (no delay,
                                                   # runs as fast as the CPU renders), this
                                                   # sequence needs a real-time HOLD to be legible
                                                   # (see _dissect_hold()), so every leg of it
                                                   # paces itself the same way for a consistent
                                                   # rotation speed throughout, not just the hold.


def render_dissection_frame(buf, preset, angle, tilt_angle, roll_angle, scale, clip_z, active_n,
                             dim_color=DISSECT_SHADE_GRAY):
    """Like orbital_view_pc.render_frame(), but for the shell-dissection
    view: (a) drops any point whose rotated depth exceeds clip_z -- the
    "cut" -- and (b) draws active_n's points (preset.shells[i] == active_n)
    at full color -- PHASE_POSITIVE_RGB/PHASE_NEGATIVE_RGB by preset.signs[i]
    where that's nonzero (an anisotropic, Hund's-rule point -- see
    atom_cloud.build_atom_point_cloud()'s docstring), its normal shell color
    otherwise (signs[i]==0: an isotropic point, no sign to show) -- every
    OTHER visible point in a single flat dim_color (not a dimmed copy of its
    own color -- the request this was built for was explicitly "other
    shells... in gray"), so one shell reads clearly while the rest stay
    visible as context. active_n=None draws everything at full shell color,
    never phase color (used for the open/close transitions and the two zoom
    legs between shells, where no single shell should be singled out yet).
    The nucleus (small square, same PROTON_SIZE/PROTON_COLOR as
    render_frame()'s) is drawn every frame regardless of active_n/clip_z --
    it's always at the origin, at depth 0, so it's never itself clipped by
    the z > clip_z cut, and stays visible through every shell of the
    sequence by design (see AtomViewApp._blit_dissection() for the "Z=n"
    text next to it).

    Two full passes over the points (not one) so the highlighted shell is
    never occluded by a coincidentally-later-drawn dim point sharing the
    same screen pixel -- there is no depth buffer here, same as
    render_frame(). No trailing persistence (render_frame()'s
    ENABLE_PERSISTENCE) -- with the clip plane re-applied fresh every frame
    to a continuously rotating cloud (see module docstring), a trailing
    glow would smear the cut edge instead of reading as motion.

    Rotation matches render_frame() exactly (yaw about Y, tilt about X,
    roll about Z) with one addition: the post-yaw-and-tilt depth `rz` (roll
    never changes depth, see draw_orbit_marker()'s comment) is computed
    too, since render_frame() never needs it but this function's clip does.
    """
    buf[:] = bytes(len(buf))

    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    xs, ys, zs, colors, shells, signs = (
        preset.xs, preset.ys, preset.zs, preset.colors, preset.shells, preset.signs)
    dr, dg, db = dim_color

    def _draw(only_n, dim):
        for i in range(len(xs)):
            if only_n is not None and shells[i] != only_n:
                continue
            if only_n is None and dim and active_n is not None and shells[i] == active_n:
                continue  # active shell's points are drawn full-color in the second pass instead

            rx1 = xs[i] * cos_yaw + zs[i] * sin_yaw
            rz1 = zs[i] * cos_yaw - xs[i] * sin_yaw
            ry2 = ys[i] * cos_tilt - rz1 * sin_tilt
            rz = ys[i] * sin_tilt + rz1 * cos_tilt
            if rz > clip_z:
                continue
            rx3 = rx1 * cos_roll - ry2 * sin_roll
            ry3 = rx1 * sin_roll + ry2 * cos_roll
            px = CENTER + round(rx3 * scale)
            py = CENTER - round(ry3 * scale)
            if 0 <= px < WIDTH and 0 <= py < HEIGHT:
                idx = (py * WIDTH + px) * 3
                if dim:
                    buf[idx], buf[idx + 1], buf[idx + 2] = dr, dg, db
                elif signs[i] > 0:
                    buf[idx], buf[idx + 1], buf[idx + 2] = cloud_common.PHASE_POSITIVE_RGB
                elif signs[i] < 0:
                    buf[idx], buf[idx + 1], buf[idx + 2] = cloud_common.PHASE_NEGATIVE_RGB
                else:
                    buf[idx], buf[idx + 1], buf[idx + 2] = colors[i]

    _draw(only_n=None, dim=(active_n is not None))
    if active_n is not None:
        _draw(only_n=active_n, dim=False)

    # Nucleus drawn LAST, on top of every electron point -- it sits at
    # depth 0 (never itself clipped by clip_z) but a densely-sampled inner
    # shell (e.g. mid-zoom on 1s, where the whole frame is close to the
    # nucleus) could otherwise paint over it if drawn first, same as
    # render_frame()'s proton marker can be overdrawn by points today. The
    # request this was built for wants the nucleus visibly present through
    # every shell, so it must win any pixel it shares with an electron point.
    proton_x0 = CENTER - PROTON_SIZE // 2
    proton_y0 = CENTER - PROTON_SIZE // 2
    pr, pg, pb = PROTON_COLOR
    for py in range(proton_y0, proton_y0 + PROTON_SIZE):
        if 0 <= py < HEIGHT:
            for px in range(proton_x0, proton_x0 + PROTON_SIZE):
                if 0 <= px < WIDTH:
                    idx = (py * WIDTH + px) * 3
                    buf[idx], buf[idx + 1], buf[idx + 2] = pr, pg, pb


class AtomPreset:
    """PC equivalent of orbital_view_pc.Preset, for one atomic number Z
    instead of one (n, ell, m) orbital. Same public shape (xs/ys/zs/colors/
    title/base_scale/zoom_amplitude/r_ref/resample()) so render_frame() and
    the fly-over/zoom-breathing loop below need no changes to accept it.
    Also carries shells/signs/config (atom_cloud.build_atom_point_cloud()'s
    extra return values) purely for the dissection view below -- normal
    rendering never reads them.
    """

    def __init__(self, z):
        print("atom: loading Z=%d (%s)..." % (z, slater.element_symbol(z)))
        t0 = time.time()

        xs, ys, zs, colors, shells, signs, config = atom_cloud.build_atom_point_cloud(z, count=N_POINTS)

        self.xs, self.ys, self.zs, self.colors, self.shells, self.signs, self.config = (
            xs, ys, zs, colors, shells, signs, config)
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
        self.root.title("Atom viewer -- PC debug (Up/Down = change element, wheel/+- = zoom, "
                        "D = dissect shells, close window to quit)")

        self.canvas = tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                 bg='black', highlightthickness=0)
        self.canvas.pack()
        self.canvas.focus_set()

        tk.Label(self.root, text="Up/Down = change element (Z). Mouse wheel or +/- = zoom. "
                                  "D = dissect shells. Close window to quit.",
                 fg='white', bg='black').pack(fill='x')

        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.photo = None  # kept alive on self; tkinter drops PhotoImages with no live reference
        self.image_id = self.canvas.create_image(0, 0, anchor='nw')

        self.z = z
        self.preset = AtomPreset(self.z)
        self._pending_z = None
        self.zoom_factor = 1.0
        self.dissecting = False
        self._pending_dissect = False

        self.canvas.bind('<Up>', lambda e: self._request_z(1))
        self.canvas.bind('<Down>', lambda e: self._request_z(-1))
        self.canvas.bind('<d>', lambda e: self._request_dissect())
        self.canvas.bind('<D>', lambda e: self._request_dissect())

        # Mouse wheel: <MouseWheel>+event.delta on Windows/Mac, Button-4/5 on
        # Linux/X11 -- binding all three covers every platform this viewer
        # runs on without needing to detect the platform explicitly.
        self.canvas.bind('<MouseWheel>', self._on_mouse_wheel)
        self.canvas.bind('<Button-4>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self.canvas.bind('<Button-5>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))

        # +/- keys: bind both the bare symbol and the keypad variant, plus
        # '=' (the un-shifted key '+' shares on a US keyboard) so zoom-in
        # doesn't require holding Shift.
        self.canvas.bind('<plus>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self.canvas.bind('<equal>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self.canvas.bind('<KP_Add>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self.canvas.bind('<minus>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))
        self.canvas.bind('<KP_Subtract>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))

        self.angle = 0.0
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi
        self.zoom_excursion_countdown = _next_zoom_excursion_countdown()

        self._fly_over(self._effective_base_scale() * INTRO_START_SCALE_FACTOR, self._effective_base_scale(),
                       INTRO_FRAMES)
        self.root.after(0, self._tick)

    def run(self):
        self.root.mainloop()

    def _request_z(self, step):
        new_z = self.z + step
        if 1 <= new_z <= slater.MAX_Z:
            self._pending_z = new_z

    def _request_dissect(self):
        # Ignored while already dissecting -- the blocking sequence below
        # pumps the tkinter event loop (root.update()) as it runs, so a
        # repeat keypress mid-sequence would otherwise queue up and
        # immediately restart the whole thing the instant this one finishes.
        if not self.dissecting:
            self._pending_dissect = True

    def _zoom_by(self, factor):
        self.zoom_factor = min(ZOOM_FACTOR_MAX, max(ZOOM_FACTOR_MIN, self.zoom_factor * factor))

    def _on_mouse_wheel(self, event):
        # event.delta is +-120-ish on Windows, small +-N on Mac -- only the
        # sign matters here.
        self._zoom_by(ZOOM_FACTOR_STEP if event.delta > 0 else 1.0 / ZOOM_FACTOR_STEP)

    def _effective_base_scale(self):
        return self.preset.base_scale * self.zoom_factor

    def _effective_zoom_amplitude(self):
        return self.preset.zoom_amplitude * self.zoom_factor

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

    def _blit_dissection(self, scale, label):
        """Like _blit(), but for dissection frames: no bounding-sphere/
        rotation marker (that marker exists to disambiguate a tumble that
        might otherwise look rotationally symmetric, see
        draw_orbit_marker()'s docstring -- the dissection view has a much
        stronger rotation cue already, the cutaway edge itself). Draws the
        current shell's label plus a "Z=n" note next to the nucleus
        instead, both requested to stay on screen through the whole
        sequence (see module docstring).
        """
        image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(self.buf), 'raw', 'RGB', 0, 1)
        draw = ImageDraw.Draw(image)
        draw_scale_bar(draw, scale / atom_cloud.ANGSTROM_PER_BOHR, "Å")
        draw.text((2, 2), self.preset.title, fill=(255, 255, 255))
        if label:
            draw.text((2, 12), label, fill=(255, 255, 0))
        draw.text((CENTER + PROTON_SIZE, CENTER - PROTON_SIZE), "Z=%d" % self.z, fill=(255, 140, 140))
        image = image.resize(DISPLAY_SIZE, Image.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.canvas.itemconfig(self.image_id, image=self.photo)

    def _dissect_tumble(self):
        """Advance self.angle/tilt_angle/roll_angle by one normal-viewing
        step. Called once per rendered frame throughout the WHOLE
        dissection sequence (ease legs and holds alike) so the cloud reads
        as continuously rotating, never paused, per the request this was
        built for -- the same three module-level step constants
        (ANGLE_STEP/TILT_ANGLE_STEP/ROLL_ANGLE_STEP) normal viewing uses,
        so the rotation speed doesn't visibly change entering or leaving
        dissection mode.
        """
        self.angle = (self.angle + ANGLE_STEP) % self.two_pi
        self.tilt_angle = (self.tilt_angle + TILT_ANGLE_STEP) % self.two_pi
        self.roll_angle = (self.roll_angle + ROLL_ANGLE_STEP) % self.two_pi

    def _dissect_ease(self, scale0, scale1, clip0, clip1, active_n, frames, label=None):
        """One eased leg of the dissection sequence: scale and clip move
        linearly from their *0 to *1 values over `frames` frames (pass the
        same value twice to hold one of them constant) while the cloud
        keeps tumbling (_dissect_tumble()) every frame. Paced to
        DISSECT_FRAME_DELAY_S (unlike _fly_over(), which has no delay and
        just runs as fast as the CPU renders) so the rotation speed here
        matches normal viewing instead of racing ahead on a fast machine.
        """
        for i in range(frames):
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = scale0 + (scale1 - scale0) * t
            clip = clip0 + (clip1 - clip0) * t
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_n)
            self._blit_dissection(scale, label)
            self.root.update()
            time.sleep(DISSECT_FRAME_DELAY_S)
            self._dissect_tumble()

    def _dissect_hold(self, scale, clip, active_n, seconds, label):
        """Real-time (not frame-count) pause on one shell, still tumbling
        every rendered frame (_dissect_tumble()) -- scale/clip/active_n stay
        fixed, only the rotation moves, so the shell's label stays legible
        for a fixed wall-clock duration regardless of how fast the host
        renders each frame.
        """
        deadline = time.time() + seconds
        while time.time() < deadline:
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_n)
            self._blit_dissection(scale, label)
            self.root.update()
            time.sleep(DISSECT_FRAME_DELAY_S)
            self._dissect_tumble()

    def _run_dissection(self):
        """The full D-key sequence -- see module docstring for the
        user-visible description. self.angle/tilt_angle/roll_angle are
        advanced in place throughout (via _dissect_tumble(), never reset),
        so _tick()'s regular per-frame update picks up the tumble exactly
        where this method leaves it once it returns.
        """
        plan = atom_cloud.shell_dissection_plan(
            self.preset.xs, self.preset.ys, self.preset.zs, self.preset.shells, self.preset.config)

        start_scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)

        # Phase 1: open the cut (nothing hidden -> z>0 hidden) at the
        # resting scale, no shell singled out yet.
        self._dissect_ease(start_scale, start_scale, DISSECT_CLIP_CLOSED, DISSECT_CLIP_OPEN,
                            active_n=None, frames=DISSECT_ORIENT_FRAMES)

        # Phase 2: outermost shell to innermost -- zoom to each shell's own
        # extent (target_scale), highlight it, hold with its label shown.
        prev_scale = start_scale
        for n, letter, subshell_str, electron_count, r_ref in plan:
            target_scale = DISSECT_TARGET_PX / max(r_ref, 1e-6)
            label = "%s shell (n=%d): %s -- %d electron%s" % (
                letter, n, subshell_str, electron_count, "" if electron_count == 1 else "s")

            self._dissect_ease(prev_scale, target_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_OPEN,
                                active_n=n, frames=DISSECT_ZOOM_FRAMES, label=label)
            self._dissect_hold(target_scale, DISSECT_CLIP_OPEN, n, DISSECT_HOLD_SECONDS, label)
            prev_scale = target_scale

        # Phase 3: zoom back out to the resting scale, still cut open but
        # with no shell singled out.
        end_scale = self._effective_base_scale()
        self._dissect_ease(prev_scale, end_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_OPEN,
                            active_n=None, frames=DISSECT_ZOOM_FRAMES)

        # Phase 4: close the cut back up.
        self._dissect_ease(end_scale, end_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_CLOSED,
                            active_n=None, frames=DISSECT_CLOSE_FRAMES)

    def _tick(self):
        if self._pending_dissect:
            self._pending_dissect = False
            self.dissecting = True
            try:
                self._run_dissection()
            finally:
                self.dissecting = False
            self.root.after(FRAME_DELAY_MS, self._tick)
            return

        if self._pending_z is not None:
            self.z = self._pending_z
            self._pending_z = None
            self.preset = AtomPreset(self.z)
            self._fly_over(self._effective_base_scale() * SWITCH_START_SCALE_FACTOR, self._effective_base_scale(),
                            SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion -- see orbital_view_pc.OrbitalViewApp._tick()'s
        # matching block for the rationale; identical here. Uses the
        # zoom-adjusted base/amplitude so excursions dive relative to
        # wherever the user has manually zoomed to, not the raw preset scale.
        self.zoom_excursion_countdown -= 1
        if self.zoom_excursion_countdown <= 0:
            current_scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)
            target_scale = self._effective_base_scale() * random.uniform(ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                                           ZOOM_EXCURSION_SCALE_MAX_FACTOR)
            self._fly_over(current_scale, target_scale, ZOOM_EXCURSION_EASE_FRAMES)
            self._fly_over(target_scale, self._effective_base_scale(), ZOOM_EXCURSION_EASE_FRAMES)
            self.zoom_angle = 0.0
            self.zoom_excursion_countdown = _next_zoom_excursion_countdown()
            self.root.after(FRAME_DELAY_MS, self._tick)
            return

        scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)
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
