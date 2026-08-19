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
import random
import sys
import time

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import atom_cloud
import clementi_radii
import cloud_common
import slater

import atom_dissection_common
from atom_dissection_common import dissection_plan

from PIL import Image, ImageDraw, ImageFont

from viewer_common import (
    CENTER, DISPLAY_SIZE, HEIGHT, WIDTH,
    INTRO_FRAMES, INTRO_START_SCALE_FACTOR,
    SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES,
    FRAME_DELAY_MS, ZOOM_ANGLE_STEP, ROLL_ANGLE_STEP,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    PROTON_SIZE, PROTON_COLOR, ELECTRON_ALPHA, ELECTRON_SIZE,
    TITLE_POS,
    render_frame, draw_orbit_marker, draw_bounding_circle, draw_scale_bar,
    draw_nucleus, rotate_yaw_tilt_roll, advance_rotation, blend_electron,
    fly_over, maybe_zoom_excursion, blit_to_canvas, find_unicode_font,
    _next_zoom_excursion_countdown,
    outer_bound_scale, inner_bound_scale, shell_count_frames,
    _HAS_NUMPY, _preset_np, _blend_points_np, _draw_nucleus_np,
)

import render_core  # shared numpy render core (also imported by web/py/web_common.py)

import tkinter as tk

# --- Cloud / defaults -------------------------------------------------------
N_POINTS = 20000
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell

# Calibrated once for THIS canvas's own CENTER (see
# atom_cloud.pixels_per_bohr_for_canvas()'s docstring for why it's a
# fraction of CENTER rather than a fixed pixel count -- the same call in
# micropython/atom_view.py uses the device's much smaller CENTER and lands
# on a different, device-appropriate PIXELS_PER_BOHR).
PIXELS_PER_BOHR = atom_cloud.pixels_per_bohr_for_canvas(CENTER)


def _valence_subshell(config):
    """Highest-l subshell among the highest-n occupied ones -- the subshell
    the Clementi-Raimondi 'atomic radius' refers to (same rule as
    pc/validate_atoms.py / pc/hfs_solver.py's valence_subshell())."""
    n_max = max(n for n, _ell, _occ in config)
    return max(((n, ell) for n, ell, _occ in config if n == n_max), key=lambda t: t[1])


def clementi_size_factor(radial_tables, z):
    """Display size calibration: CR literature radius / the radial model's
    valence-subshell mode radius.

    With the SPARC-atomSFE tables (`radial_tables`): the LDA eigenvalues are
    NIST-exact (~1e-6 Ha, pc/nist_compare_atomsfe.py) but the valence
    orbitals are more diffuse than the HF-based Clementi-Raimondi reference
    (LDA self-interaction error -- worst for light elements: H ~2.2x,
    period 2 ~1.7x, Fe ~1.1x). Without tables (hydrogenic model): the
    z_eff substitution already matches CR for the lightest elements but
    drifts for alkali/transition metals and strongly past Z=54 (Slater
    fallback) -- atom_size_calib.py carries the hydrogenic factors
    (shared with the micropython and C++ ports).

    Either way the returned factor rescales the rendered cloud so the
    valence subshell's mode radius lands on the CR literature value while
    the internal shell structure stays the model's own. Returns 1.0 when
    the literature value or the model entry is unavailable (Z>92, missing
    subshell/table entry).
    """
    if radial_tables is None:
        # Hydrogenic model: generated factor table (tools/atom_size_calib_gen.py),
        # shared with the micropython/web/C++ ports via atom_dissection_common.py.
        return atom_dissection_common.default_size_factor(z)
    lit = clementi_radii.CLEMENTI_RADIUS_PM.get(z)
    if not lit:
        return 1.0
    try:
        config = slater.electron_configuration(z)
        n, ell = _valence_subshell(config)
        mode_pm = radial_tables.source(z, n, ell).mode_radius() * atom_cloud.PM_PER_BOHR
    except (KeyError, ValueError):
        return 1.0
    if mode_pm <= 0.0:
        return 1.0
    return lit / mode_pm

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
DISSECT_ORIENT_FRAMES = 55   # base frames to ease the clip open/closed (see shell_count_frames())
DISSECT_ZOOM_FRAMES = 55     # base frames to ease camera scale from one stop to the next
# Mirrors the device's kDissectFlySpeedPmPerSec/kDissectFlyMinMs pacing
# (halved/doubled respectively, i.e. 2x slower) -- only the per-shell zoom
# legs are slowed; the open/close legs keep their own constants (a PC-only
# extra with no device equivalent).
DISSECT_ZOOM_SLOWDOWN = 2.0
DISSECT_HOLD_SECONDS = 2     # real-time pause on each shell with its label shown
DISSECT_CLOSE_FRAMES = 100   # base frames to ease the cut shut on the way back to the resting scale
DISSECT_FRAMES_PER_SHELL = 8  # extra frames added to every eased leg per subshell beyond the first
                               # (see shell_count_frames()) -- a heavier element's dissection runs
                               # longer, matching its bigger outer-to-innermost-shell zoom range
# Paces every leg of the dissection to the same rotation speed normal viewing
# uses -- unlike fly_over()'s transitions (no delay, run as fast as the CPU
# renders), this sequence needs a real-time HOLD to be legible, so all legs
# pace themselves the same way for a consistent rotation speed throughout.
DISSECT_FRAME_DELAY_S = FRAME_DELAY_MS / 1000.0

# --- Dissection HUD ---------------------------------------------------------
# Port of src/views/atom_view.cpp's drawDissectTitle(): a big subshell label
# ("2p") with a plain-size caption ("Fe (2/5)", the element symbol)
# underneath, and the electron count as a small "<occ>e-" note in the
# top-right corner, kept visually distinct from the orbital name.
DISSECT_BIG_FONT_SIZE = 72
DISSECT_CAPTION_FONT_SIZE = 28
DISSECT_OCC_FONT_SIZE = 24
DISSECT_TITLE_COLOR = (255, 255, 255)
DISSECT_OCC_MARGIN_PX = 8

# --- Element-switch intro (name slide-in + 0.5Hz flash) ---------------------
# Port of src/views/atom_view.cpp's scrollElementIntro(): before switching to a new
# element, slide its Italian name in from the right over a big pale symbol
# watermark with a "Z=n" caption, hold, then flash the name on/off once at
# 0.5Hz (1s visible, 1s blank) instead of sliding back out.
ELEMENT_INTRO_SLIDE_PX = 12     # device kElementIntroPxPerFrame=6 on 240 -> 2x on the 480 buffer
ELEMENT_INTRO_HOLD_S = 0.5      # device kElementIntroHoldMs
ELEMENT_INTRO_FLASH_HALF_PERIOD_S = 1.0  # 0.5Hz = 2s period (1s name-visible, 1s blank)
ELEMENT_INTRO_NAME_MARGIN_PX = 40
ELEMENT_INTRO_SYMBOL_COLOR = (90, 90, 100)  # device kElementIntroSymbolColor
ELEMENT_INTRO_SYMBOL_FONT_SIZE = 170
ELEMENT_INTRO_Z_FONT_SIZE = 28

# --- Dissection intro card --------------------------------------------------
# Port of src/views/atom_view.cpp's showElectronConfigIntro(): a static 3-line
# "Configurazione / elettronica / <nome>" title card over a tiled dim "e-"
# backdrop, held before the dissection sequence itself starts.
DISSECT_INTRO_LINE1 = "Configurazione"
DISSECT_INTRO_LINE2 = "elettronica"
DISSECT_INTRO_WORD_FONT_SIZE = 64   # ~2x the device's kFontLarge-at-scale-2
DISSECT_INTRO_LINE_GAP_PX = 100     # device kDissectIntroLineGapPx=50 on 240 -> 2x
DISSECT_INTRO_START_Y = 100
DISSECT_INTRO_HOLD_S = 0.9          # device kDissectIntroHoldMs=900
DISSECT_INTRO_COLOR = (255, 210, 60)  # device kAccentColor
DISSECT_INTRO_BG_COLOR = (55, 55, 55)  # device kDissectIntroBgColor
DISSECT_INTRO_BG_SPACING = (88, 68)    # device 44/34 on 240 -> 2x
DISSECT_INTRO_BG_START = 12
DISSECT_INTRO_BG_FONT_SIZE = 24

# --- Idle auto-advance ------------------------------------------------------
# Port of the device's atom_view.cpp idle logic (kIdleJumpUs=60s): with no
# input for 60s, either dissect the CURRENT element (coin flip, at most once
# per element) or jump to a random different element; both use the exact
# same animations as manual navigation.
IDLE_JUMP_SECONDS = 60.0
IDLE_DISSECT_PROBABILITY = 0.5

# --- Dissection HUD colors (kept for reference; title now drawn via PIL) ----
Z_NOTE_COLOR = (255, 140, 140)

def _render_dissection_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale, clip_z,
                                 active_subshell, dim_color):
    """numpy fast path of render_dissection_frame() -- same two-pass
    structure and color rules as the Python version below, vectorized:
    pass 1 draws every point (except the active subshell when one is singled
    out) in `dim_color` at ELECTRON_ALPHA; pass 2 draws only the active
    subshell's points in full color (phase colors where a sign is defined,
    its true SHELL_RGB where signs[i]==0 -- not preset.colors, see the
    Python path's comment) at ACTIVE_SUBSHELL_ALPHA (opaque). Mutates `buf`
    via a zero-copy numpy view. Implementation shared with the web port
    lives in render_core.render_dissection_frame_np().
    """
    render_core.render_dissection_frame_np(
        buf, preset, arr, angle, tilt_angle, roll_angle, scale,
        clip_z, active_subshell, dim_color,
        WIDTH, HEIGHT, CENTER, ELECTRON_SIZE, ELECTRON_ALPHA, ACTIVE_SUBSHELL_ALPHA,
        PROTON_SIZE, PROTON_COLOR)


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

    With numpy installed this takes the vectorized fast path (the same
    _blend_points_np core as render_frame(), run twice with the two passes'
    color rules -- see _render_dissection_frame_np); the pure-Python loop
    below is the no-numpy fallback.
    """
    if _HAS_NUMPY and ELECTRON_SIZE in (1, 2):
        arr = _preset_np(preset)
        if arr is not None:
            _render_dissection_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale,
                                        clip_z, active_subshell, dim_color)
            return

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
                # comment for the blend itself. Drawn at ELECTRON_SIZE via the
                # shared blend_electron(), same as the normal view. The nucleus
                # (drawn below, after both passes) is always fully opaque.
                blend_electron(buf, px, py, cr, cg, cb, alpha)

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


def draw_atom_title(draw, x, y, z, config, outer_n=None, outer_ell=None):
    """Draw the atom title ('Ca (Z=20) ') in white, then each subshell of its
    electron configuration ('1s2 2s2 2p6 ...') color-coded by shell --
    atom_cloud.SHELL_RGB[n], the same colors the cloud's own points use -- so
    the on-screen legend and the rendered cloud read as one color language.

    (outer_n, outer_ell), when given (AtomPreset.outer_n/outer_ell -- the
    subshell with the largest MEASURED radius in this cloud, not just the
    last entry in `config`), gets its segment brightened toward white the
    same way that subshell's own points are (atom_cloud._brighten_outer_shell,
    same helper/factor -- reused directly rather than a second constant that
    could drift) -- otherwise the near-white valence points and their still-
    fully-saturated legend color would visibly disagree.

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
        if n == outer_n and ell == outer_ell:
            color = atom_cloud._brighten_outer_shell(color)
        draw.text((cursor_x, y), segment, fill=color)
        cursor_x += draw.textlength(segment)


def make_atom_preset(z, radial_tables=None):
    """AtomPreset for this viewer's own N_POINTS/PIXELS_PER_BOHR, with the
    display-size factor this module's clementi_size_factor() computes
    (tables-based for the screened-potential tables, hydrogenic-factor --
    shared with the micropython/web/C++ ports -- for the no-tables path).
    See atom_dissection_common.AtomPreset's docstring for the shared shape.
    """
    return atom_dissection_common.AtomPreset(
        z, N_POINTS, PIXELS_PER_BOHR,
        size_factor=clementi_size_factor(radial_tables, z), radial_tables=radial_tables)


class AtomViewApp:
    """tkinter app driving render_frame() over AtomPreset -- a trimmed-down
    copy of orbital_view_pc.OrbitalViewApp with the nudge-based preset switch
    replaced by Up/Down changing Z. Kept as its own class rather than
    subclassing: the two differ exactly in the input-handling bit, and
    inheritance would need overriding most of _tick() anyway.

    Standalone (`root=None`) creates and owns its own window, as before. Run
    from pc/launcher.py instead, `root`/`canvas`/`image_id` are the shared
    ones the chooser screen already created, and `on_exit` is the callback
    that shows the chooser again -- see _request_exit()/stop(), and
    orbital_view_pc.OrbitalViewApp's matching docstring.
    """

    def __init__(self, z=DEFAULT_Z, root=None, canvas=None, image_id=None, on_exit=None,
                 radial_tables=None):
        self.owns_root = root is None
        self.root = root or tk.Tk()
        if self.owns_root:
            self.root.title("Atom viewer -- PC debug (Up/Down = change element, wheel/+- = zoom, "
                            "D = dissect orbitals, Esc/close window to quit)")

        self.canvas = canvas or tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                           bg='black', highlightthickness=0)
        if canvas is None:
            self.canvas.pack()
        self.canvas.focus_set()

        if self.owns_root:
            tk.Label(self.root, text="Up/Down = change element (Z). Mouse wheel or +/- = zoom. "
                                      "D = dissect orbitals. Esc/close window to quit.",
                     fg='white', bg='black').pack(fill='x')

        # aborted/on_exit/_bound_sequences: the shared Escape-to-return
        # protocol fly_over()/maybe_zoom_excursion() check and stop() uses
        # -- see orbital_view_pc.OrbitalViewApp's matching fields.
        self.aborted = False
        self.on_exit = on_exit
        self._bound_sequences = []

        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.photo = None  # kept alive on self; tkinter drops PhotoImages with no live reference
        self.image_id = image_id if image_id is not None else self.canvas.create_image(0, 0, anchor='nw')

        self.z = z
        self.radial_tables = radial_tables
        # Z range: the whole project is limited to Z<=92 (slater.MAX_DISPLAY_Z
        # -- the SPARC-atomSFE tables' hard cap), for every port including
        # this one's hydrogenic model; the Z=93..118 data stays in slater but
        # navigation never goes there. The tables' own coverage is also
        # respected in case a partial npz is loaded.
        self._max_z = min(
            radial_tables.z_list[-1]
            if radial_tables is not None and hasattr(radial_tables, 'z_list')
            else slater.MAX_Z,
            slater.MAX_DISPLAY_Z)
        self.preset = make_atom_preset(self.z, radial_tables)
        self._pending_z = None
        self.zoom_factor = 1.0
        self.dissecting = False
        self._pending_dissect = False
        # Any movement during a dissection closes it and returns to the
        # element -- set by the input handlers below while a dissection
        # runs; the sequence checks it every frame and returns to normal
        # viewing. Unlike `aborted` (Escape, exits the whole app), this
        # only aborts the dissection.
        self.abort_dissection = False
        # Idle auto-advance state (see the constants above): clock reset by
        # every input, plus the once-per-element dissection budget.
        self.last_activity = time.time()
        self.idle_dissected_this_element = False

        self._bind('<Up>', lambda e: self._request_z(1))
        self._bind('<Down>', lambda e: self._request_z(-1))
        self._bind('<d>', lambda e: self._request_dissect())
        self._bind('<D>', lambda e: self._request_dissect())
        # Bound on the WINDOW, not the canvas: canvas.bind() only fires
        # while the canvas itself holds keyboard focus, which a "go back"
        # shortcut shouldn't depend on. root.bind() fires regardless of
        # which child widget has focus, as long as the window does.
        self.root.bind('<Escape>', self._request_exit)

        # Mouse wheel: <MouseWheel>+event.delta on Windows/Mac, Button-4/5 on
        # Linux/X11 -- binding all three covers every platform this viewer
        # runs on without detecting the platform explicitly.
        self._bind('<MouseWheel>', self._on_mouse_wheel)
        self._bind('<Button-4>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self._bind('<Button-5>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))

        # +/- keys: bare symbol, keypad variant, and '=' (the un-shifted key
        # '+' shares on a US keyboard) so zoom-in doesn't require Shift.
        self._bind('<plus>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self._bind('<equal>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self._bind('<KP_Add>', lambda e: self._zoom_by(ZOOM_FACTOR_STEP))
        self._bind('<minus>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))
        self._bind('<KP_Subtract>', lambda e: self._zoom_by(1.0 / ZOOM_FACTOR_STEP))

        self.angle = 0.0
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi
        self.zoom_excursion_countdown = _next_zoom_excursion_countdown()

        # Dissection HUD fonts (big shell label, caption, corner occ note).
        self._dissect_big_font = find_unicode_font(DISSECT_BIG_FONT_SIZE) or ImageFont.load_default(
            size=DISSECT_BIG_FONT_SIZE)
        self._dissect_caption_font = find_unicode_font(DISSECT_CAPTION_FONT_SIZE) or ImageFont.load_default(
            size=DISSECT_CAPTION_FONT_SIZE)
        self._dissect_occ_font = find_unicode_font(DISSECT_OCC_FONT_SIZE) or ImageFont.load_default(
            size=DISSECT_OCC_FONT_SIZE)

        fly_over(self, self._effective_base_scale() * INTRO_START_SCALE_FACTOR, self._effective_base_scale(),
                 INTRO_FRAMES)
        # If Escape fired during THIS fly-over, no _tick() has ever been
        # scheduled yet -- _tick() is the only other place that calls
        # stop(), so without this check an abort here would never actually
        # take effect (the app would just freeze, aborted=True forever).
        if self.aborted:
            self.stop()
        else:
            self.root.after(0, self._tick)

    def run(self):
        self.root.mainloop()

    def _bind(self, sequence, handler):
        self.canvas.bind(sequence, handler)
        self._bound_sequences.append(sequence)

    def _request_exit(self, event=None):
        """See orbital_view_pc.OrbitalViewApp._request_exit()'s docstring --
        same reasoning applies here, including why a dissection in progress
        (a much longer blocking sequence than a fly-over) is safe to
        interrupt this way: _dissect_ease()/_dissect_hold() check `aborted`
        every iteration too (see those methods).
        """
        print("atom: Escape pressed, aborted=True")
        self.aborted = True

    def stop(self):
        """See orbital_view_pc.OrbitalViewApp.stop()'s docstring -- same
        reasoning and same "only ever called from _tick()" contract.
        """
        print("atom: stop() -- unbinding %d sequence(s), on_exit=%r, owns_root=%r" % (
            len(self._bound_sequences), self.on_exit, self.owns_root))
        for sequence in self._bound_sequences:
            self.canvas.unbind(sequence)
        self.root.unbind('<Escape>')  # bound on root, not canvas -- see __init__
        if self.on_exit is not None:
            self.on_exit()
        elif self.owns_root:
            self.root.destroy()

    def _request_z(self, step):
        self.last_activity = time.time()  # any input resets the idle clock
        if self.dissecting:
            # Any movement during a dissection aborts it back to the full
            # element -- the gesture is consumed by the abort, not queued as
            # a pending Z change (matching the device's tilt handling).
            self.abort_dissection = True
            return
        new_z = self.z + step
        if 1 <= new_z <= self._max_z:
            self._pending_z = new_z

    def _request_dissect(self):
        self.last_activity = time.time()
        if self.dissecting:
            # D during a dissection counts as "movement" too -- abort.
            self.abort_dissection = True
            return
        # Ignored while already dissecting -- the blocking sequence pumps the
        # tkinter event loop (root.update()) as it runs, so a repeat keypress
        # mid-sequence would otherwise queue up and immediately restart the
        # whole thing the instant this one finishes.
        if not self.dissecting:
            self._pending_dissect = True

    def _zoom_by(self, factor):
        self.last_activity = time.time()
        if self.dissecting:
            # Wheel/+- during a dissection aborts it (see _request_z()).
            self.abort_dissection = True
            return
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
            draw_atom_title(draw, TITLE_POS[0], TITLE_POS[1], self.z, self.preset.config,
                             self.preset.outer_n, self.preset.outer_ell)
        blit_to_canvas(self, overlays)

    def _blit_dissection(self, scale, r_ref, title):
        """Like _blit(), but for dissection frames: the rotating spoke/text
        part of draw_orbit_marker() is skipped -- just its plain gray
        bounding-circle outline (draw_bounding_circle(), neutral
        BOUNDING_SPHERE_COLOR), so the reference sphere's silhouette stays
        visible even when the active subshell's dimming makes the actual
        points hard to see. `title` is a (big_label, caption, occ) tuple or
        None: big_label ("2p") in a large font, `caption` ("Fe (2/5)", the
        element symbol) plain-size underneath it, and a small "<occ>e-" note in the
        top-right corner, kept distinct from the orbital name -- the
        device's drawDissectTitle() layout. Also draws a "Z=n" note next
        to the nucleus.
        """
        def overlays(draw):
            draw_bounding_circle(draw, r_ref, scale)
            draw_scale_bar(draw, scale / atom_cloud.PM_PER_BOHR, "pm")
            if title is not None:
                big_label, caption, occ = title
                draw.text(TITLE_POS, big_label, fill=DISSECT_TITLE_COLOR, font=self._dissect_big_font)
                draw.text((TITLE_POS[0], TITLE_POS[1] + DISSECT_BIG_FONT_SIZE + 6),
                          caption, fill=DISSECT_TITLE_COLOR, font=self._dissect_caption_font)
                occ_text = "%de-" % occ
                occ_x = WIDTH - draw.textlength(occ_text, font=self._dissect_occ_font) - DISSECT_OCC_MARGIN_PX
                draw.text((occ_x, DISSECT_OCC_MARGIN_PX), occ_text, fill=DISSECT_TITLE_COLOR,
                          font=self._dissect_occ_font)
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
                       frames, title=None, full_tumble=False):
        """One eased leg of the dissection sequence: scale and clip move
        linearly from their *0 to *1 values over `frames` frames (pass the
        same value twice to hold one constant) while the cloud keeps tumbling.
        Paced to DISSECT_FRAME_DELAY_S (unlike fly_over(), which has no
        delay and runs as fast as the CPU renders) so the rotation speed here
        matches normal viewing instead of racing ahead. `title` is a
        (big_label, caption, occ) tuple for _blit_dissection(), or None.

        full_tumble=True keeps yaw/tilt advancing too (advance_rotation(),
        the same normal-viewing tumble as outside the dissection sequence)
        instead of _dissect_tumble()'s roll-only freeze -- only safe while
        the clip is CLOSED throughout the leg (clip0==clip1==
        DISSECT_CLIP_CLOSED, nothing actually being cut), i.e. the opening
        leg before the cut starts opening and the closing leg after it's
        shut again. _run_dissection() uses it there so the camera keeps
        rotating exactly as it was the instant D was pressed / exactly as
        normal viewing resumes after, instead of visibly locking to
        roll-only right at the start/end of the sequence.
        """
        for i in range(frames):
            if self.aborted or self.abort_dissection:  # see _request_exit()'s docstring
                return
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = scale0 + (scale1 - scale0) * t
            clip = clip0 + (clip1 - clip0) * t
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_subshell)
            self._blit_dissection(scale, r_ref, title)
            self.root.update()
            time.sleep(DISSECT_FRAME_DELAY_S)
            if full_tumble:
                advance_rotation(self)
            else:
                self._dissect_tumble()

    def _dissect_hold(self, scale, clip, active_subshell, r_ref, seconds, title):
        """Real-time (not frame-count) pause on one subshell, still tumbling
        every rendered frame -- scale/clip/active_subshell stay fixed, so the
        subshell's label stays legible for a fixed wall-clock duration
        regardless of how fast the host renders each frame. `title` is a
        (big_label, caption, occ) tuple for _blit_dissection().
        """
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.aborted or self.abort_dissection:  # see _request_exit()'s docstring
                return
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_subshell)
            self._blit_dissection(scale, r_ref, title)
            self.root.update()
            time.sleep(DISSECT_FRAME_DELAY_S)
            self._dissect_tumble()

    # --- Blocking helper scenes (element intro, dissection intro) -------------

    def _sleep_responsive(self, seconds):
        """Real-time sleep that keeps the window responsive and checks
        `aborted` (Escape) -- the device's vTaskDelay() equivalent, with
        root.update() so key events still fire during the intro holds.
        Returns False if aborted mid-sleep.
        """
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.aborted:
                return False
            self.root.update()
            # root.update() can overrun the remaining time -- clamp to >= 0.
            time.sleep(max(0.0, min(0.05, deadline - time.time())))
        return True

    @staticmethod
    def _pick_fit_font(text, max_width, sizes, fallback):
        """Largest font size from `sizes` whose rendered width of `text`
        fits within `max_width` (device pickNameScale()'s idea: size the
        name to fit, biggest first); `fallback` if none fit.
        """
        probe = ImageDraw.Draw(Image.new('RGB', (1, 1)))
        for size in sizes:
            font = find_unicode_font(size) or ImageFont.load_default(size=size)
            if probe.textlength(text, font=font) <= max_width:
                return font
        return fallback

    def _element_intro(self, new_z):
        """Slide `name` (the element's Italian name) in from the right over a
        big, static, pale watermark of `symbol`, pause centered, then flash
        the name on/off once at 0.5Hz (1s visible, 1s blank) -- the PC
        counterpart of the device's scrollElementIntro() (name slide-in +
        hold + flash, shown before switching to a new element).

        Layout matches the device's current version: the name sits at 2/3 of
        the canvas height (clear of the centered symbol watermark) and the
        "Z=<z>" caption in the upper 1/3, so the element name stays clearly
        readable against the watermark.
        """
        name = slater.element_name_it(new_z)
        symbol = slater.element_symbol(new_z)
        z_label = "Z=%d" % new_z
        name_font = self._pick_fit_font(
            name, WIDTH - 2 * ELEMENT_INTRO_NAME_MARGIN_PX,
            (96, 72, 56, 44, 36, 28),
            find_unicode_font(48) or ImageFont.load_default(size=48))
        symbol_font = find_unicode_font(ELEMENT_INTRO_SYMBOL_FONT_SIZE) or ImageFont.load_default(
            size=ELEMENT_INTRO_SYMBOL_FONT_SIZE)
        z_font = find_unicode_font(ELEMENT_INTRO_Z_FONT_SIZE) or ImageFont.load_default(
            size=ELEMENT_INTRO_Z_FONT_SIZE)

        probe = ImageDraw.Draw(Image.new('RGB', (1, 1)))
        name_width = probe.textlength(name, font=name_font)
        center_x = (WIDTH - name_width) / 2
        symbol_y = (HEIGHT - ELEMENT_INTRO_SYMBOL_FONT_SIZE) // 2 - 20
        name_y = 2 * HEIGHT // 3     # 2/3 height -- the name's focal slot
        z_y = HEIGHT // 3            # upper 1/3 -- the "Z=xx" caption

        def render_at(x, show_name=True):
            self.buf[:] = bytes(len(self.buf))  # clear -- device renderAt() fills black first
            image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(self.buf), 'raw', 'RGB', 0, 1)
            draw = ImageDraw.Draw(image)
            sw = probe.textlength(symbol, font=symbol_font)
            draw.text(((WIDTH - sw) / 2, symbol_y), symbol, font=symbol_font, fill=ELEMENT_INTRO_SYMBOL_COLOR)
            if show_name:
                draw.text((x, name_y), name, font=name_font, fill=(255, 255, 255))
                zw = probe.textlength(z_label, font=z_font)
                draw.text(((WIDTH - zw) / 2, z_y), z_label, font=z_font, fill=(255, 255, 255))
            self.buf[:] = image.tobytes()
            blit_to_canvas(self, lambda d: None)  # full-screen intro frame, no overlays
            self.root.update()

        # Slide in from the right edge to centered, then hold.
        x = WIDTH
        while x > center_x:
            if self.aborted:
                return
            render_at(x)
            x -= ELEMENT_INTRO_SLIDE_PX
        render_at(center_x)  # land exactly centered -- the loop above may step past it
        if not self._sleep_responsive(ELEMENT_INTRO_HOLD_S):
            return

        # Flash tail: 0.5Hz = one 1s-blank / 1s-visible cycle (device
        # kElementIntroFlashHalfPeriodMs).
        render_at(center_x, show_name=False)
        if not self._sleep_responsive(ELEMENT_INTRO_FLASH_HALF_PERIOD_S):
            return
        render_at(center_x, show_name=True)
        self._sleep_responsive(ELEMENT_INTRO_FLASH_HALF_PERIOD_S)

    def _dissection_intro(self):
        """Static 3-line "Configurazione / elettronica / <nome>" title card
        over a tiled dim "e-" backdrop, held DISSECT_INTRO_HOLD_S before the
        dissection sequence itself starts -- the PC counterpart of the
        device's showElectronConfigIntro() (same layout: three accent-colored
        lines, the element name at the same size as the two fixed words unless
        too wide, over a dim "e-" grid).
        """
        name = slater.element_name_it(self.z)
        word_font = find_unicode_font(DISSECT_INTRO_WORD_FONT_SIZE) or ImageFont.load_default(
            size=DISSECT_INTRO_WORD_FONT_SIZE)
        name_font = self._pick_fit_font(
            name, WIDTH - 40, (DISSECT_INTRO_WORD_FONT_SIZE, 40, 28),
            word_font)
        bg_font = find_unicode_font(DISSECT_INTRO_BG_FONT_SIZE) or ImageFont.load_default(
            size=DISSECT_INTRO_BG_FONT_SIZE)
        probe = ImageDraw.Draw(Image.new('RGB', (1, 1)))

        self.buf[:] = bytes(len(self.buf))
        image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(self.buf), 'raw', 'RGB', 0, 1)
        draw = ImageDraw.Draw(image)

        # Tiled dim "e-" backdrop (device drawElectronBackdrop(), spacing
        # doubled for the 480 buffer).
        for y in range(DISSECT_INTRO_BG_START, HEIGHT, DISSECT_INTRO_BG_SPACING[1]):
            for x in range(DISSECT_INTRO_BG_START, WIDTH, DISSECT_INTRO_BG_SPACING[0]):
                draw.text((x, y), "e-", font=bg_font, fill=DISSECT_INTRO_BG_COLOR)

        y1 = DISSECT_INTRO_START_Y
        y2 = y1 + DISSECT_INTRO_LINE_GAP_PX
        y3 = y2 + DISSECT_INTRO_LINE_GAP_PX
        for line, y, font in ((DISSECT_INTRO_LINE1, y1, word_font),
                              (DISSECT_INTRO_LINE2, y2, word_font),
                              (name, y3, name_font)):
            w = probe.textlength(line, font=font)
            draw.text(((WIDTH - w) / 2, y), line, font=font, fill=DISSECT_INTRO_COLOR)

        self.buf[:] = image.tobytes()
        blit_to_canvas(self, lambda d: None)
        self.root.update()
        self._sleep_responsive(DISSECT_INTRO_HOLD_S)

    def _dissection_plan(self):
        """The current element's subshell dissection plan (see
        atom_dissection_common.dissection_plan()) -- shared by
        _run_dissection() and the idle auto-advance's can-dissect check.
        """
        return dissection_plan(self.preset)

    @staticmethod
    def _random_z_excluding(current):
        """Random Z in [1, MAX_Z], guaranteed != current (device
        randomIndexExcluding(z-1, kMaxZ)+1).
        """
        offset = 1 + random.randrange(self._max_z - 1)
        return 1 + (current - 1 + offset) % self._max_z

    def _switch_to_element(self, new_z):
        """Element-name intro + fly-over switch to `new_z` -- the PC
        counterpart of the device's switchToElement() lambda
        (scrollElementIntro() then flyOver()), shared by Up/Down and the idle
        random jump so both transitions look identical.
        """
        print("atom: switching element Z=%d -> %d (%s)" % (self.z, new_z, slater.element_name_it(new_z)))
        self._element_intro(new_z)
        if self.aborted:
            return
        self.z = new_z
        self.preset = make_atom_preset(new_z, self.radial_tables)
        self.idle_dissected_this_element = False  # fresh element -- fresh idle dissection budget
        fly_over(self, self._effective_base_scale() * SWITCH_START_SCALE_FACTOR, self._effective_base_scale(),
                 SWITCH_TRANSITION_FRAMES)
        self.zoom_angle = 0.0
        self.zoom_excursion_countdown = _next_zoom_excursion_countdown()

    def _run_dissection(self):
        """The full D-key sequence -- see module docstring for the
        user-visible description. Yaw/tilt/roll are advanced in place
        throughout (never reset), so _tick()'s regular per-frame update picks
        up the tumble exactly where this method leaves it.

        The whole sequence is bracketed by the same shared zoom envelope
        maybe_zoom_excursion() dives through (see viewer_common.py):
        eased out to outer_scale (self.preset.r_ref x ZOOM_OUTER_RADIUS_FACTOR,
        an unambiguous "outside" overview) before the cut opens, and eased in
        to inner_scale (self.preset.inner_r_ref -- the first/innermost
        shell's own radius -- x ZOOM_INNER_RADIUS_FACTOR, deeper than that
        shell's own extent) on the last subshell, then back out through the
        same two stops before returning control to normal viewing. The
        subshells IN BETWEEN keep the original per-shell framing (each one's
        own r_ref filling DISSECT_TARGET_PX), so only the two ends of the
        journey are pinned to the guaranteed bounds. Every eased leg is
        stretched by shell_count_frames() so heavier elements (more
        subshells, a bigger outer-to-inner range) get a proportionally
        longer sequence instead of feeling rushed.

        The actual phase-by-phase plan (scale/clip/timing per leg) is
        computed once by atom_dissection_common.build_dissection_steps(),
        shared with web/py/web_atom.py's dissection_sequence() -- this
        method just executes the resulting steps with its own blocking
        _dissect_ease()/_dissect_hold() primitives.
        """
        # Title card first -- "Configurazione / elettronica / <nome>" over
        # the tiled "e-" backdrop (device showElectronConfigIntro()).
        self._dissection_intro()
        if self.aborted or self.abort_dissection:
            return

        plan = self._dissection_plan()

        shell_count = self.preset.shell_count
        orient_frames = shell_count_frames(DISSECT_ORIENT_FRAMES, DISSECT_FRAMES_PER_SHELL, shell_count)
        # Shell-to-shell hops paced ~2x slower -- see DISSECT_ZOOM_SLOWDOWN.
        zoom_frames = int(shell_count_frames(DISSECT_ZOOM_FRAMES, DISSECT_FRAMES_PER_SHELL, shell_count)
                          * DISSECT_ZOOM_SLOWDOWN)
        close_frames = shell_count_frames(DISSECT_CLOSE_FRAMES, DISSECT_FRAMES_PER_SHELL, shell_count)

        resting_scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)
        outer_scale = outer_bound_scale(self.preset.r_ref)
        inner_scale = inner_bound_scale(self.preset.inner_r_ref)

        steps = atom_dissection_common.build_dissection_steps(
            plan, self.preset.r_ref, resting_scale, outer_scale, inner_scale,
            orient_frames, zoom_frames, close_frames, DISSECT_HOLD_SECONDS,
            DISSECT_TARGET_PX, DISSECT_CLIP_OPEN, DISSECT_CLIP_CLOSED,
            slater.element_symbol(self.z))

        # Every step below is followed by `if self.aborted or
        # self.abort_dissection: return` -- Escape (see _request_exit()) and
        # any movement (see the input handlers) can only interrupt a step
        # BETWEEN whole _dissect_ease()/_dissect_hold() calls (each of those
        # already breaks out of its own loop promptly, but control still
        # returns here afterward), so without this check an abort mid-sequence
        # would otherwise fall through into the NEXT step instead of
        # stopping.
        for step in steps:
            if step[0] == 'ease':
                _, scale0, scale1, clip0, clip1, active_subshell, r_ref, frames, title, full_tumble = step
                self._dissect_ease(scale0, scale1, clip0, clip1, active_subshell, r_ref,
                                    frames, title=title, full_tumble=full_tumble)
            else:
                _, scale, clip, active_subshell, r_ref, seconds, title = step
                self._dissect_hold(scale, clip, active_subshell, r_ref, seconds, title)
            if self.aborted or self.abort_dissection:
                return

    def _tick(self):
        if self.aborted:
            print("atom: _tick() saw aborted -- calling stop()")
            self.stop()
            return

        if self._pending_dissect:
            self._pending_dissect = False
            self.dissecting = True
            try:
                self._run_dissection()
            finally:
                self.dissecting = False
            # A movement-aborted dissection (abort_dissection) just returns to
            # normal viewing on the current element; only Escape stops the app.
            self.abort_dissection = False
            if self.aborted:
                self.stop()
                return
            self.root.after(FRAME_DELAY_MS, self._tick)
            return

        if self._pending_z is not None:
            new_z = self._pending_z
            self._pending_z = None
            self._switch_to_element(new_z)
            if self.aborted:
                self.stop()
                return

        # Idle auto-advance: 60s without input -> either dissect the CURRENT
        # element (coin flip, once per element -- the device's
        # idleDissectedThisElement budget) or jump to a random different
        # element; both reuse the manual animations (device atom_view.cpp idle
        # logic).
        if time.time() - self.last_activity > IDLE_JUMP_SECONDS:
            plan = self._dissection_plan()
            can_dissect = not self.idle_dissected_this_element and len(plan) > 0
            if can_dissect and random.random() < IDLE_DISSECT_PROBABILITY:
                print("atom: idle %.0fs+ -- dissecting current element (Z=%d, %d shells)" % (
                    IDLE_JUMP_SECONDS, self.z, len(plan)))
                self.dissecting = True
                try:
                    self._run_dissection()
                finally:
                    self.dissecting = False
                self.abort_dissection = False
                self.idle_dissected_this_element = True
                self.zoom_angle = 0.0
                self.zoom_excursion_countdown = _next_zoom_excursion_countdown()
            else:
                new_z = self._random_z_excluding(self.z)
                print("atom: idle %.0fs+ -- jumping to random element Z=%d" % (IDLE_JUMP_SECONDS, new_z))
                self._switch_to_element(new_z)
            self.last_activity = time.time()
            if self.aborted:
                self.stop()
                return
            self.root.after(FRAME_DELAY_MS, self._tick)
            return

        # Random zoom excursion -- same helper as OrbitalViewApp._tick(); uses
        # the zoom-adjusted base/amplitude and scale_factor so dives are
        # relative to wherever the user has manually zoomed to, and the
        # preset's own outer/inner shell radii and subshell count so every
        # dive reaches the first shell's own depth with duration paced to
        # how many subshells this element has.
        if maybe_zoom_excursion(self, self._effective_base_scale(), self._effective_zoom_amplitude(),
                                 self.preset.r_ref, self.preset.inner_r_ref,
                                 shell_count=self.preset.shell_count, scale_factor=self.zoom_factor):
            if self.aborted:
                self.stop()
            return

        scale = self._effective_base_scale() + self._effective_zoom_amplitude() * math.sin(self.zoom_angle)
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale)
        self._blit(scale)

        advance_rotation(self)
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run(z=DEFAULT_Z, radial_tables=None):
    AtomViewApp(z, radial_tables=radial_tables).run()


if __name__ == '__main__':
    import os
    import sys
    # Allow: python3 atom_view_pc.py [Z] [--model hfs [--tables PATH]]
    _model = 'hydrogenic'
    _tables = None
    _z = DEFAULT_Z
    _argv = sys.argv[1:]
    while _argv:
        a = _argv.pop(0)
        if a == '--model':
            _model = _argv.pop(0)
        elif a == '--tables':
            _tables = _argv.pop(0)
        else:
            _z = int(a)
    _rt = None
    if _model == 'hfs':
        import hfs_tables
        _rt = hfs_tables.load(_tables or hfs_tables.DEFAULT_TABLES)
    run(_z, radial_tables=_rt)
