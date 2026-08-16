"""PC-only viewer for atom_cloud.py's multi-electron point clouds -- same
tumbling-camera rendering as orbital_view_pc.py's hydrogen-preset viewer
(both built on pc/viewer_common.py's shared render_frame()/draw_orbit_marker()
etc.), but cycling through atomic number Z instead of (n, ell, m) presets.
See pc/README.md's "Multi-electron atoms" section for the model and controls.

    python3 pc/atom_main.py [Z]

Up/Down changes Z live (same fly-over transition as a preset switch); D runs
a one-shot dissection sequence (see AtomViewApp._run_dissection()): the cloud
keeps spinning (roll only -- see AtomViewApp._dissect_tumble()) while the
near half stays clipped away (camera-space clip, but since roll never
changes clip depth, the SAME half stays excluded for the whole sequence
instead of sweeping through fresh material), then the camera zooms subshell
by subshell from outermost to innermost, dimming the others to gray and
phase-coloring the active subshell wherever a sign is defined
(atom_cloud.build_atom_point_cloud()'s `signs`), and finally zooms back out
and un-cuts. No point turnover here -- AtomPreset.resample() is a no-op,
since cloud_common's turnover only knows single-orbital distributions and
this cloud is a mixture of several subshells.

Both this view and the normal (non-dissecting) one also draw a plain gray
bounding-circle outline (draw_bounding_circle(), the neutral
BOUNDING_SPHERE_COLOR -- deliberately not shell-colored) tracking the
reference sphere, so the eye has a recognizable sphere shape to track even
when the active subshell's dimming makes the actual points hard to see.
"""

import math
import os
import sys
import time

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import atom_cloud
import cloud_common
import slater

from viewer_common import (
    CENTER, DISPLAY_SIZE, HEIGHT, WIDTH,
    INTRO_FRAMES, INTRO_START_SCALE_FACTOR,
    SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES,
    FRAME_DELAY_MS, ZOOM_ANGLE_STEP, ROLL_ANGLE_STEP,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    PROTON_SIZE, ELECTRON_ALPHA,
    TITLE_POS, SUBTITLE_POS,
    render_frame, draw_orbit_marker, draw_bounding_circle, draw_scale_bar,
    draw_nucleus, rotate_yaw_tilt_roll, advance_rotation,
    fly_over, maybe_zoom_excursion, blit_to_canvas,
    _next_zoom_excursion_countdown,
)

import tkinter as tk

# --- Cloud / defaults -------------------------------------------------------
N_POINTS = 10000
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell

# --- Manual zoom (mouse wheel / +- keys) ------------------------------------
# A persistent multiplier on top of preset.base_scale, independent of the
# automatic zoom-breathing/excursion animation. Multiplicative step (not
# additive) so each notch/keypress feels like the same relative zoom whether
# already zoomed in or out.
ZOOM_FACTOR_MIN = 0.15
ZOOM_FACTOR_MAX = 8.0
ZOOM_FACTOR_STEP = 1.1

# --- Shell-dissection sequence (D key; see _run_dissection()) ---------------
DISSECT_TARGET_PX = 100.0  # on-screen p90 radius each shell's disc is zoomed to fill
DISSECT_SHADE_GRAY = (70, 70, 70)  # flat gray for every non-active shell's points
ACTIVE_SUBSHELL_ALPHA = 1.0  # opaque -- the exploded subshell ignores ELECTRON_ALPHA
DISSECT_CLIP_OPEN = 0.0     # clip threshold that hides rotated-z > 0 (the "cut" is open)
DISSECT_CLIP_CLOSED = 1.0e6  # clip threshold no real point can exceed (nothing hidden)
DISSECT_ORIENT_FRAMES = 40   # ease the clip open while the cloud keeps tumbling
DISSECT_ZOOM_FRAMES = 40     # ease camera scale from one shell to the next
DISSECT_HOLD_SECONDS = 2     # real-time pause on each shell with its label shown
DISSECT_CLOSE_FRAMES = 80    # ease the cut shut on the way back to the resting scale
# Paces every leg of the dissection to the same rotation speed normal viewing
# uses -- unlike fly_over()'s transitions (no delay, run as fast as the CPU
# renders), this sequence needs a real-time HOLD to be legible, so all legs
# pace themselves the same way for a consistent rotation speed throughout.
DISSECT_FRAME_DELAY_S = FRAME_DELAY_MS / 1000.0

# --- Dissection HUD ---------------------------------------------------------
DISSECT_LABEL_COLOR = (255, 255, 0)
Z_NOTE_COLOR = (255, 140, 140)

def render_dissection_frame(buf, preset, angle, tilt_angle, roll_angle, scale, clip_z, active_subshell,
                             dim_color=DISSECT_SHADE_GRAY):
    """Like orbital_view_pc.render_frame(), but for the dissection view:
    (a) drops any point whose rotated depth exceeds clip_z -- the "cut" --
    and (b) highlights active_subshell (an (n, ell) pair): its points draw
    at full color -- PHASE_POSITIVE_RGB/PHASE_NEGATIVE_RGB by preset.signs[i]
    where nonzero, its normal shell color where signs[i]==0 -- while every
    other visible point draws in a single flat dim_color. active_subshell=None
    draws everything at full shell color (used for the open/close transitions
    and the zoom legs, where none should be singled out).

    Two full passes over the points (not one) so the highlighted subshell is
    never occluded by a later-drawn dim point sharing the same pixel -- there
    is no depth buffer here. No trailing persistence: with the clip plane
    re-applied fresh to a rotating cloud, a glow would smear the cut edge.
    The nucleus is drawn last, always at depth 0 so never clipped, and stays
    visible through every subshell by design.

    Rotation matches render_frame() exactly, plus the post-yaw-and-tilt depth
    `rz` (see rotate_yaw_tilt_roll()) which this function's clip needs.
    """
    buf[:] = bytes(len(buf))

    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    xs, ys, zs, colors, shells, ells, signs = (
        preset.xs, preset.ys, preset.zs, preset.colors, preset.shells, preset.ells, preset.signs)
    dr, dg, db = dim_color

    def _draw(only_subshell, dim, alpha):
        for i in range(len(xs)):
            if only_subshell is not None and (shells[i], ells[i]) != only_subshell:
                continue
            if (only_subshell is None and dim and active_subshell is not None
                    and (shells[i], ells[i]) == active_subshell):
                continue  # active subshell's points are drawn full-color in the second pass instead

            rx3, ry3, rz = rotate_yaw_tilt_roll(xs[i], ys[i], zs[i],
                                                cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll)
            if rz > clip_z:
                continue
            px = CENTER + round(rx3 * scale)
            py = CENTER - round(ry3 * scale)
            if 0 <= px < WIDTH and 0 <= py < HEIGHT:
                idx = (py * WIDTH + px) * 3
                if dim:
                    cr, cg, cb = dr, dg, db
                elif signs[i] > 0:
                    cr, cg, cb = cloud_common.PHASE_POSITIVE_RGB
                elif signs[i] < 0:
                    cr, cg, cb = cloud_common.PHASE_NEGATIVE_RGB
                elif only_subshell is not None:
                    # Spotlighting THIS subshell specifically: show its true
                    # SHELL_RGB color, not preset.colors[i] -- atom_cloud.py
                    # brightens/dims that array for the MERGED view (outer
                    # subshell boosted, everything else penalized, see its
                    # Coloring docstring), which would otherwise make an
                    # inner shell render dull-but-opaque here instead of
                    # actually lighting up on its own turn.
                    n = shells[i]
                    cr, cg, cb = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
                else:
                    cr, cg, cb = colors[i]
                # Alpha-blended (context/dim pass) or opaque (active-subshell
                # pass, alpha=1.0) -- see orbital_view_pc.ELECTRON_ALPHA's
                # comment for the blend itself. The nucleus (drawn below,
                # after both passes) is always fully opaque.
                buf[idx] = buf[idx] + int((cr - buf[idx]) * alpha)
                buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * alpha)
                buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * alpha)

    _draw(only_subshell=None, dim=(active_subshell is not None), alpha=ELECTRON_ALPHA)
    if active_subshell is not None:
        # Opaque, not alpha-blended: the shell currently being explained
        # should render solid/crisp, not partially see-through.
        _draw(only_subshell=active_subshell, dim=False, alpha=ACTIVE_SUBSHELL_ALPHA)

    # Nucleus drawn LAST, on top of every electron point -- it sits at depth
    # 0 (never clipped by clip_z) but a densely-sampled inner shell could
    # otherwise paint over it; it must win any pixel it shares so it stays
    # visible through every shell.
    draw_nucleus(buf)


def draw_atom_title(draw, x, y, z, config):
    """Draw the atom title ('Ca (Z=20) ') in white, then each subshell of its
    electron configuration ('1s2 2s2 2p6 ...') color-coded by shell --
    atom_cloud.SHELL_RGB[n], the same colors the cloud's own points use -- so
    the on-screen legend and the rendered cloud read as one color language.

    PIL's ImageDraw has no multi-color single-call text primitive, so this
    draws segment by segment, advancing x by each segment's measured width
    (draw.textlength()).
    """
    prefix = "%s (Z=%d) " % (slater.element_symbol(z), z)
    draw.text((x, y), prefix, fill=(255, 255, 255))
    cursor_x = x + draw.textlength(prefix)
    for n, ell, occ in config:
        segment = "%s%d " % (slater.subshell_label(n, ell), occ)
        color = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
        draw.text((cursor_x, y), segment, fill=color)
        cursor_x += draw.textlength(segment)


class AtomPreset:
    """PC equivalent of orbital_view_pc.Preset, for one atomic number Z
    instead of one (n, ell, m) orbital. Same public shape (xs/ys/zs/colors/
    title/base_scale/zoom_amplitude/r_ref/resample()) so render_frame() and
    the fly-over/zoom-breathing loop accept it unchanged; also carries
    shells/ells/signs/config (from atom_cloud.build_atom_point_cloud()) for
    the dissection view.
    """

    def __init__(self, z):
        print("atom: loading Z=%d (%s)..." % (z, slater.element_symbol(z)))
        t0 = time.time()

        xs, ys, zs, colors, shells, ells, signs, config = atom_cloud.build_atom_point_cloud(z, count=N_POINTS)

        self.xs, self.ys, self.zs, self.colors, self.shells, self.ells, self.signs, self.config = (
            xs, ys, zs, colors, shells, ells, signs, config)
        self.title = atom_cloud.title_for_atom(z, config)
        # Same plan atom_cloud.outer_subshell_r_ref() would compute internally
        # -- called directly here instead so the outermost subshell's own
        # measured radius is available (what defines the atom's physical
        # size; see outer_subshell_r_ref()'s docstring).
        outer_plan = atom_cloud.subshell_dissection_plan(xs, ys, zs, shells, ells, config)
        r_ref = outer_plan[0][5] if outer_plan else 1.0
        self.base_scale, self.zoom_amplitude, self.r_ref = atom_cloud.scale_for_atom(
            r_ref, atom_cloud.PIXELS_PER_BOHR)

        print("atom: %s loaded in %.2fs, scale=%.1f" % (
            slater.element_symbol(z), time.time() - t0, self.base_scale))

    def resample(self, count):
        pass  # static cloud -- see module docstring


class AtomViewApp:
    """tkinter app driving render_frame() over AtomPreset -- a trimmed-down
    copy of orbital_view_pc.OrbitalViewApp with the nudge-based preset switch
    replaced by Up/Down changing Z. Kept as its own class rather than
    subclassing: the two differ exactly in the input-handling bit, and
    inheritance would need overriding most of _tick() anyway.
    """

    def __init__(self, z=DEFAULT_Z):
        self.root = tk.Tk()
        self.root.title("Atom viewer -- PC debug (Up/Down = change element, wheel/+- = zoom, "
                        "D = dissect orbitals, close window to quit)")

        self.canvas = tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                 bg='black', highlightthickness=0)
        self.canvas.pack()
        self.canvas.focus_set()

        tk.Label(self.root, text="Up/Down = change element (Z). Mouse wheel or +/- = zoom. "
                                  "D = dissect orbitals. Close window to quit.",
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
        # runs on without detecting the platform explicitly.
        self.canvas.bind('<MouseWheel>', self._on_mouse_wheel)
        self.canvas.bind('<Button-4>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self.canvas.bind('<Button-5>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))

        # +/- keys: bare symbol, keypad variant, and '=' (the un-shifted key
        # '+' shares on a US keyboard) so zoom-in doesn't require Shift.
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

        fly_over(self, self._effective_base_scale() * INTRO_START_SCALE_FACTOR, self._effective_base_scale(),
                 INTRO_FRAMES)
        self.root.after(0, self._tick)

    def run(self):
        self.root.mainloop()

    def _request_z(self, step):
        new_z = self.z + step
        if 1 <= new_z <= slater.MAX_Z:
            self._pending_z = new_z

    def _request_dissect(self):
        # Ignored while already dissecting -- the blocking sequence pumps the
        # tkinter event loop (root.update()) as it runs, so a repeat keypress
        # mid-sequence would otherwise queue up and immediately restart the
        # whole thing the instant this one finishes.
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
        def overlays(draw):
            # Neutral gray bounding circle (default BOUNDING_SPHERE_COLOR --
            # deliberately not shell-colored).
            draw_orbit_marker(draw, self.preset.r_ref, scale, self.angle, self.tilt_angle, self.roll_angle,
                               marker_text=slater.element_symbol(self.z))
            # Scale (px per Bohr radius, THIS frame -- varies with zoom
            # breathing/excursions) -> px per picometer, so the bar always
            # reflects the camera's current zoom, not just the resting one.
            draw_scale_bar(draw, scale / atom_cloud.PM_PER_BOHR, "pm")
            draw_atom_title(draw, TITLE_POS[0], TITLE_POS[1], self.z, self.preset.config)
        blit_to_canvas(self, overlays)

    def _blit_dissection(self, scale, r_ref, label):
        """Like _blit(), but for dissection frames: the rotating spoke/text
        part of draw_orbit_marker() is skipped -- just its plain gray
        bounding-circle outline (draw_bounding_circle(), neutral
        BOUNDING_SPHERE_COLOR), so the reference sphere's silhouette stays
        visible even when the active subshell's dimming makes the actual
        points hard to see. Also draws the current shell's label and a
        "Z=n" note next to the nucleus.
        """
        def overlays(draw):
            draw_bounding_circle(draw, r_ref, scale)
            draw_scale_bar(draw, scale / atom_cloud.PM_PER_BOHR, "pm")
            draw_atom_title(draw, TITLE_POS[0], TITLE_POS[1], self.z, self.preset.config)
            if label:
                draw.text(SUBTITLE_POS, label, fill=DISSECT_LABEL_COLOR)
            draw.text((CENTER + PROTON_SIZE, CENTER - PROTON_SIZE), "Z=%d" % self.z, fill=Z_NOTE_COLOR)
        blit_to_canvas(self, overlays)

    def _dissect_tumble(self):
        """Advance roll only -- called every rendered frame throughout the
        WHOLE dissection sequence (ease legs and holds alike) so the cloud
        keeps visibly, continuously spinning without ever pausing, but
        without yaw/tilt carrying the clip plane across new material.
        rotate_yaw_tilt_roll() computes rz (the clip's depth) from yaw and
        tilt only -- roll never changes it -- so freezing yaw/tilt for the
        whole sequence keeps exactly the same half of the cloud excluded
        throughout, camera-space clip plane and cloud rotating together as
        one rigid unit ("rotate casually but still inside this plane")
        instead of the clip sweeping through fresh material as the object
        tumbles underneath it.
        """
        self.roll_angle = (self.roll_angle + ROLL_ANGLE_STEP) % self.two_pi

    def _dissect_ease(self, scale0, scale1, clip0, clip1, active_subshell, r_ref,
                       frames, label=None):
        """One eased leg of the dissection sequence: scale and clip move
        linearly from their *0 to *1 values over `frames` frames (pass the
        same value twice to hold one constant) while the cloud keeps tumbling.
        Paced to DISSECT_FRAME_DELAY_S (unlike fly_over(), which has no
        delay and runs as fast as the CPU renders) so the rotation speed here
        matches normal viewing instead of racing ahead.
        """
        for i in range(frames):
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = scale0 + (scale1 - scale0) * t
            clip = clip0 + (clip1 - clip0) * t
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_subshell)
            self._blit_dissection(scale, r_ref, label)
            self.root.update()
            time.sleep(DISSECT_FRAME_DELAY_S)
            self._dissect_tumble()

    def _dissect_hold(self, scale, clip, active_subshell, r_ref, seconds, label):
        """Real-time (not frame-count) pause on one subshell, still tumbling
        every rendered frame -- scale/clip/active_subshell stay fixed, so the
        subshell's label stays legible for a fixed wall-clock duration
        regardless of how fast the host renders each frame.
        """
        deadline = time.time() + seconds
        while time.time() < deadline:
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_subshell)
            self._blit_dissection(scale, r_ref, label)
            self.root.update()
            time.sleep(DISSECT_FRAME_DELAY_S)
            self._dissect_tumble()

    def _run_dissection(self):
        """The full D-key sequence -- see module docstring for the
        user-visible description. Yaw/tilt/roll are advanced in place
        throughout (never reset), so _tick()'s regular per-frame update picks
        up the tumble exactly where this method leaves it.
        """
        plan = atom_cloud.subshell_dissection_plan(
            self.preset.xs, self.preset.ys, self.preset.zs, self.preset.shells, self.preset.ells,
            self.preset.config)

        start_scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)

        # Phase 1: open the cut (nothing hidden -> z>0 hidden) at the resting
        # scale, no subshell singled out yet.
        self._dissect_ease(start_scale, start_scale, DISSECT_CLIP_CLOSED, DISSECT_CLIP_OPEN,
                            active_subshell=None, r_ref=self.preset.r_ref,
                            frames=DISSECT_ORIENT_FRAMES)

        # Phase 2: outermost subshell to innermost -- zoom to each subshell's
        # own extent (target_scale), highlight it, hold with its label shown.
        prev_scale = start_scale
        for n, ell, letter, subshell_str, electron_count, r_ref in plan:
            target_scale = DISSECT_TARGET_PX / max(r_ref, 1e-6)
            label = "%s: n=%d, l=%d (%s shell) -- %d electron%s" % (
                subshell_str, n, ell, letter, electron_count, "" if electron_count == 1 else "s")

            self._dissect_ease(prev_scale, target_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_OPEN,
                                active_subshell=(n, ell), r_ref=r_ref,
                                frames=DISSECT_ZOOM_FRAMES, label=label)
            self._dissect_hold(target_scale, DISSECT_CLIP_OPEN, (n, ell), r_ref,
                               DISSECT_HOLD_SECONDS, label)
            prev_scale = target_scale

        # Phase 3: zoom back out to the resting scale, still cut open but
        # with no subshell singled out.
        end_scale = self._effective_base_scale()
        self._dissect_ease(prev_scale, end_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_OPEN,
                            active_subshell=None, r_ref=self.preset.r_ref,
                            frames=DISSECT_ZOOM_FRAMES)

        # Phase 4: close the cut back up.
        self._dissect_ease(end_scale, end_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_CLOSED,
                            active_subshell=None, r_ref=self.preset.r_ref,
                            frames=DISSECT_CLOSE_FRAMES)

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
            fly_over(self, self._effective_base_scale() * SWITCH_START_SCALE_FACTOR, self._effective_base_scale(),
                     SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion -- same helper as OrbitalViewApp._tick(); uses
        # the zoom-adjusted base/amplitude so dives are relative to wherever
        # the user has manually zoomed to.
        if maybe_zoom_excursion(self, self._effective_base_scale(), self._effective_zoom_amplitude()):
            return

        scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale)
        self._blit(scale)

        advance_rotation(self)
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run(z=DEFAULT_Z):
    AtomViewApp(z).run()


if __name__ == '__main__':
    run(int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_Z)
