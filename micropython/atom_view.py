"""Multi-electron atom point-cloud animation for the ESP32-S3 panel -- the
device counterpart of pc/atom_view_pc.py, built the same way orbital_view.py
is: atom_cloud.py supplies the model (multi-electron point cloud, shared
unmodified with the PC viewer), device_render_common.py supplies the Q8
fixed-point/viper rendering, framebuf/ST7789 blitting, fly-over/zoom-
excursion camera, and nudge/IMU plumbing (all shared with orbital_view.py
too). What's left here is genuinely atom-specific: AtomPresetState
(shell-by-n point coloring, no point-turnover -- atom_cloud's cloud is
static, see its module docstring), _draw_atom_title() (framebuf counterpart
of pc/atom_view_pc.py's draw_atom_title() -- element symbol huge top-left,
Z number huge top-right, electron configuration as a secondary large-scale
line, matching src/views/atom_view.cpp's renderAtomFrame() layout with the
electron-configuration legend kept as an MPY-only addition), N_POINTS
(smaller than the PC viewer's 10000, same device rendering budget as
orbital_view.py's 3000), a shell-dissection sequence (D nudge or idle
auto-advance -- see _run_dissection() below, MicroPython port of that
file's runDissectionSequence()), and a run() loop that lets nudge step the
atomic number Z instead of cycling a fixed preset list.

Not the default boot animation (CLAUDE.md's M1-M4 roadmap is single-electron
hydrogen orbitals) -- run this the same way corner_test.py is run, as a
standalone alternate entry point instead of editing main.py. After copying
micropython/. to the board (see main.py's docstring for the mpremote
incantation that actually works on this board):
    mpremote connect <port> exec "import atom_view; atom_view.run()"
"""

import math
import random
import time

import array
import framebuf

import atom_cloud
import device_render_common as drc
import display as display_mod
import hfs_atom_size_calib
import hfs_radial_tables
import slater

# Loaded once (not per element switch, see AtomPresetState below): only the
# header/index (a few KB) lands in RAM here, each subshell's own u(r) is read
# from hfs_tables.bin on demand -- see hfs_radial_tables.py's module
# docstring. Screened-potential (HFS/atomSFE) tables are the radial model
# unconditionally for z<=92 (same as src/physics/atom_cloud.cpp -- see
# pc/RUN_HFS.md section 5's NIST 92/92 configuration cross-check for why no
# hydrogenic fallback is needed here).
_HFS_TABLES = hfs_radial_tables.load()

WIDTH = drc.WIDTH
HEIGHT = drc.HEIGHT
CENTER = drc.CENTER

N_POINTS = 3000  # same device render budget as orbital_view.py's cloud_common.N_POINTS
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell

# Calibrated once for THIS panel's own CENTER (see
# atom_cloud.pixels_per_bohr_for_canvas()'s docstring for why it's a
# fraction of CENTER rather than a fixed pixel count -- the same call in
# pc/atom_view_pc.py uses the PC debug window's much larger CENTER and lands
# on a different, PC-appropriate PIXELS_PER_BOHR).
PIXELS_PER_BOHR = atom_cloud.pixels_per_bohr_for_canvas(CENTER)

FPS_UPDATE_INTERVAL = 50
FPS_TEXT_POS = (2, 2)

_GREEN_YELLOW = drc.encode_color565(173, 255, 41)  # matches Display::kColorGreenYellow (Z-number corner label)

# --- Shell-dissection sequence (D nudge or idle auto-advance) ----------------------------------
#
# MicroPython port of src/views/atom_view.cpp's runDissectionSequence(): auto-peel through every
# occupied subshell outer-to-inner, easing the camera to frame each one with its own label, real-
# time hold, then ease back to the full atom. Simplified vs. the C++ side in two ways, both scope
# trims rather than missing pieces: no tiled-electron-backdrop intro card (Italian-name data this
# project's C++ build has isn't ported to MicroPython), and no "buzz" hidden-points flicker during
# the sequence (atom_view.py's steady-state loop never uses buzz either, see AtomPresetState's
# docstring) -- the core interactive behavior (peel/hold/label, movement aborts it) is what's
# ported.
_DISSECT_DIM_COLOR = drc.encode_color565(70, 70, 70)  # matches kDissectDimColor
_DISSECT_OCC_COLOR = drc.encode_color565(40, 80, 210)  # Display::kColorOrbitalBlue
_DISSECT_HOLD_MS = 2000                # matches kDissectHoldUs
_DISSECT_FLY_SPEED_PM_PER_SEC = 30.0   # matches kDissectFlySpeedPmPerSec
_DISSECT_FLY_MIN_MS = 700              # matches kDissectFlyMinMs


def _dissection_ranges(shells, ells, plan):
    """(start_index, count) per `plan` entry (atom_cloud.subshell_dissection_plan()'s
    outer-to-inner sorted list) -- build_atom_point_cloud() writes each subshell's points as one
    contiguous run (in slater.electron_configuration()'s config order, see that function's
    docstring), so this is a single linear scan per subshell, not a search. Computed once per
    element load, not per frame or per dissection level.
    """
    n_points = len(shells)
    ranges = []
    for entry in plan:
        n, ell = entry[0], entry[1]
        start = -1
        count = 0
        for i in range(n_points):
            if shells[i] == n and ells[i] == ell:
                if start < 0:
                    start = i
                count += 1
        ranges.append((start if start >= 0 else 0, count))
    return ranges


def _build_dissect_colors(plan, ranges, level, total_count):
    """array('H') RGB565 colors for dissect `level` (1..len(plan)): the newly-revealed outermost
    remaining subshell (plan[level-1]) gets its own bright SHELL_RGB color; every subshell deeper
    than it (still visible) gets a flat dim gray; every subshell peeled away (outer of level-1)
    stays black -- i.e. invisible once alpha-blended against the persistence-faded background
    (see device_render_common.py's render_points() blend). MicroPython port of
    src/views/atom_view.cpp's buildDissectGroups(), adapted from PointGroup ranges to a flat
    per-point colors array since this project's device render path colors atoms per-point, not
    per-group (see atom_cloud.py's module docstring).
    """
    colors = array.array('H', bytes(2 * total_count))  # zero-filled -> black -> invisible
    for rank in range(level - 1, len(plan)):
        start, count = ranges[rank]
        if count == 0:
            continue
        if rank == level - 1:
            n = plan[rank][0]
            r, g, b = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
            fill = drc.encode_color565(r, g, b)
        else:
            fill = _DISSECT_DIM_COLOR
        for i in range(start, start + count):
            colors[i] = fill
    return colors


def _fly_duration_ms(from_rref, to_rref):
    """Real-time ease duration between two shells' own reference radii, matching
    src/views/atom_view.cpp's dissectFlyDurationMs() (constant pm/s, floored so a near-zero hop
    still eases briefly instead of cutting instantly).
    """
    distance_pm = abs(to_rref - from_rref) * atom_cloud.PM_PER_BOHR
    ms = int(distance_pm / _DISSECT_FLY_SPEED_PM_PER_SEC * 1000.0)
    return ms if ms > _DISSECT_FLY_MIN_MS else _DISSECT_FLY_MIN_MS


class _DissectPreset:
    """Lightweight stand-in for AtomPresetState, for one dissection level: same
    xs_fx/ys_fx/zs_fx (points never move during dissection, only which ones are visible and in
    what color), a level-specific colors array (see _build_dissect_colors()), and a
    level-specific title (big shell label + element/level caption + occupancy corner note,
    drawn all together in draw_title() since draw_corner_label() below is unused here).
    """

    def __init__(self, xs_fx, ys_fx, zs_fx, colors, title_fn):
        self.xs_fx = xs_fx
        self.ys_fx = ys_fx
        self.zs_fx = zs_fx
        self.colors = colors
        self._title_fn = title_fn

    def draw_title(self, fb, buf, x, y, text_color):
        self._title_fn(fb, buf, x, y, text_color)

    def draw_corner_label(self, fb, buf, text_color):
        pass  # occupancy note is drawn as part of draw_title() above instead


def _ease_scale_timed(d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                      start_scale, end_scale, duration_ms, detector):
    """Like device_render_common.fly_over(), but eases over REAL time
    (time.ticks_ms()) instead of a fixed frame count, and aborts early
    (returning completed=False) if `detector` reports any nudge mid-ease --
    MicroPython port of src/views/atom_view.cpp's easeScaleTimed().
    `detector=None` disables the abort check (used for the sequence's own
    closing "back to full view" leg, matching that function's default
    nullptr -- the closing action should not itself be interruptible).

    Returns (completed, angle, tilt_angle, roll_angle).
    """
    two_pi = 2 * math.pi
    t0 = time.ticks_ms()
    frame_salt = 0
    while True:
        if detector is not None and detector.poll_raw() is not None:
            return False, angle, tilt_angle, roll_angle

        elapsed = time.ticks_diff(time.ticks_ms(), t0)
        t = elapsed / duration_ms if duration_ms > 0 else 1.0
        if t > 1.0:
            t = 1.0
        scale = start_scale + (end_scale - start_scale) * t

        drc.render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, frame_salt, 0)
        preset.draw_title(fb, buf, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], text_color)
        drc.draw_scale_bar(fb, buf, scale / atom_cloud.PM_PER_BOHR, "pm", scale_bar_color, text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        angle += drc.ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += drc.TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += drc.ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi
        frame_salt += 1
        if t >= 1.0:
            break
        time.sleep_ms(1)
    return True, angle, tilt_angle, roll_angle


def _hold_dissect(d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                  scale, hold_ms, detector):
    """Real-time hold at a fixed `scale` while continuing to tumble, aborting early on any nudge
    -- MicroPython port of runDissectionSequence()'s per-level hold loop (renderDissectFrame()
    calls inline). Returns (completed, angle, tilt_angle, roll_angle).
    """
    two_pi = 2 * math.pi
    t0 = time.ticks_ms()
    frame_salt = 0
    while time.ticks_diff(time.ticks_ms(), t0) < hold_ms:
        if detector is not None and detector.poll_raw() is not None:
            return False, angle, tilt_angle, roll_angle

        drc.render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, frame_salt, 0)
        preset.draw_title(fb, buf, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], text_color)
        drc.draw_scale_bar(fb, buf, scale / atom_cloud.PM_PER_BOHR, "pm", scale_bar_color, text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        angle += drc.ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += drc.TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += drc.ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi
        frame_salt += 1
        time.sleep_ms(1)
    return True, angle, tilt_angle, roll_angle


def _run_dissection(d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                    detector):
    """Auto-peel through every occupied subshell, outer to inner, then ease back to the full
    atom -- MicroPython port of src/views/atom_view.cpp's runDissectionSequence() (see this
    module's docstring for the two scope trims vs. that version). Returns the running
    (angle, tilt_angle, roll_angle) so the caller's steady-state loop continues smoothly after.
    """
    plan = preset.dissect_plan
    if not plan:
        return angle, tilt_angle, roll_angle
    ranges = preset.dissect_ranges
    symbol = slater.element_symbol(preset.z)
    total_count = len(preset.xs_fx)

    scale = preset.base_scale
    prev_rref = plan[0][5]  # seeded to the outermost shell's own radius, same reasoning as
                             # the C++ side: level 1 is already framed by the full-atom view,
                             # so the first hop computes a near-zero distance.

    for level in range(1, len(plan) + 1):
        n, ell, _letter, _subshell_str, occ, rref = plan[level - 1]
        colors = _build_dissect_colors(plan, ranges, level, total_count)
        big_label = slater.subshell_label(n, ell)
        caption = "%s (%d/%d)" % (symbol, level, len(plan))
        occ_text = "%de-" % occ

        def title_fn(fb_, buf_, x, y, text_color_, big_label=big_label, caption=caption, occ_text=occ_text):
            drc.draw_text_scaled(fb_, buf_, x, y, big_label, text_color_, drc.FONT_SCALE_HUGE)
            drc.draw_text_scaled(fb_, buf_, x, y + 8 * drc.FONT_SCALE_HUGE + 2, caption, text_color_,
                                 drc.FONT_SCALE_LARGE)
            occ_w = drc.text_width_scaled(occ_text, drc.FONT_SCALE_LARGE)
            drc.draw_text_scaled(fb_, buf_, WIDTH - occ_w - 1, 1, occ_text, _DISSECT_OCC_COLOR, drc.FONT_SCALE_LARGE)

        level_preset = _DissectPreset(preset.xs_fx, preset.ys_fx, preset.zs_fx, colors, title_fn)
        target_scale, _amp, _rr = atom_cloud.scale_for_atom(rref, PIXELS_PER_BOHR)
        fly_ms = _fly_duration_ms(prev_rref, rref)
        print("atom: dissecting shell %s (level %d/%d, fly %dms)" % (big_label, level, len(plan), fly_ms))

        completed, angle, tilt_angle, roll_angle = _ease_scale_timed(
            d, fb, buf, level_preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
            scale, target_scale, fly_ms, detector)
        scale = target_scale
        prev_rref = rref
        if not completed:
            print("atom: dissection aborted -- movement detected mid-fly")
            break

        completed, angle, tilt_angle, roll_angle = _hold_dissect(
            d, fb, buf, level_preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
            scale, _DISSECT_HOLD_MS, detector)
        if not completed:
            print("atom: dissection aborted -- movement detected during hold")
            break

    return_ms = _fly_duration_ms(prev_rref, plan[0][5])
    _completed, angle, tilt_angle, roll_angle = _ease_scale_timed(
        d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
        scale, preset.base_scale, return_ms, None)
    return angle, tilt_angle, roll_angle


# --- Full-atom render/state (steady-state view, not dissection) --------------------------------

def _draw_atom_title(fb, buf, x, y, z, config, text_color):
    """Device (framebuf) counterpart of pc/atom_view_pc.py's
    draw_atom_title(): element symbol at FONT_SCALE_HUGE (matching
    src/views/atom_view.cpp's drawAtomTitle(), kFontHuge), then each
    subshell of its electron configuration ('1s2 2s2 2p2 ...') at
    FONT_SCALE_LARGE, colored by shell (atom_cloud.SHELL_RGB[n] -- the same
    colors the point cloud itself uses) -- this configuration line is an
    MPY-only addition (the C++ live view shows just the symbol + a separate
    Z-number corner label, see AtomPresetState.draw_corner_label() below;
    kept here since it's informative and this panel has the room).

    Wraps to a new line instead of running off the right edge when a
    segment would cross `x + WIDTH`. `cursor_x > x`'s check never wraps
    mid-segment on the first segment of a line -- an over-wide single
    segment still gets drawn (and clipped by framebuf itself) rather than
    wrapping forever.
    """
    drc.draw_text_scaled(fb, buf, x, y, slater.element_symbol(z), text_color, drc.FONT_SCALE_HUGE)

    cursor_x = x
    cursor_y = y + 8 * drc.FONT_SCALE_HUGE + 2
    line_height = 8 * drc.FONT_SCALE_LARGE + 2
    for n, ell, occ in config:
        segment = "%s%d " % (slater.subshell_label(n, ell), occ)
        seg_width = drc.text_width_scaled(segment, drc.FONT_SCALE_LARGE)
        if cursor_x > x and cursor_x + seg_width > drc.WIDTH:
            cursor_x = x
            cursor_y += line_height
        r, g, b = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
        drc.draw_text_scaled(fb, buf, cursor_x, cursor_y, segment, drc.encode_color565(r, g, b), drc.FONT_SCALE_LARGE)
        cursor_x += seg_width


class AtomPresetState:
    """Everything one loaded element needs to render: Q8 fixed-point
    coordinates and shell-colored, encoded colors -- no resample() (unlike
    orbital_view.PresetState), since atom_cloud's cloud is built once and
    stays static (see atom_cloud.py's module docstring for why: it is a
    mixture of several subshells, and cloud_common's point-turnover only
    knows a single orbital's distribution). Also builds the shell-dissection
    plan/ranges once here (see _run_dissection() above) rather than lazily
    on first use, so a D nudge/idle dissection never has to pay that cost
    mid-sequence.
    """

    def __init__(self, z):
        print("atom: loading Z=%d (%s)..." % (z, slater.element_symbol(z)))
        t0 = time.ticks_ms()

        xs, ys, zs, colors_rgb, shells, ells, _signs, config = atom_cloud.build_atom_point_cloud(
            z, count=N_POINTS, radial_tables=_HFS_TABLES)

        # Clementi-Raimondi display-size calibration (see
        # hfs_atom_size_calib.py): rescale the whole cloud so the valence
        # subshell's mode radius lands on the literature value -- the same
        # table-based per-element factor the device (src/physics/atom_size_calib.h)
        # uses, since both now render through the HFS tables above. NOT
        # micropython/atom_size_calib.py -- that one is hydrogenic-model
        # factors, shared with the PC viewer's hydrogenic default and the
        # web viewer, see tools/atom_size_calib_gen.py. Scaling preserves the
        # internal shell structure; only the atom's overall size changes
        # (and hence its size relative to other elements, which is what the
        # calibration is for).
        f = hfs_atom_size_calib.FACTOR[z - 1]
        if f != 1.0:
            xs = array.array('f', (v * f for v in xs))
            ys = array.array('f', (v * f for v in ys))
            zs = array.array('f', (v * f for v in zs))

        self.xs_fx = drc.to_fixed(xs)
        self.ys_fx = drc.to_fixed(ys)
        self.zs_fx = drc.to_fixed(zs)
        self.colors = drc.encode_rgb_colors(colors_rgb)

        self.z = z
        self.config = config
        self.shells = shells
        self.ells = ells

        r_ref = atom_cloud.outer_subshell_r_ref(xs, ys, zs, shells, ells, config)
        self.base_scale, self.zoom_amplitude, self.r_ref = atom_cloud.scale_for_atom(r_ref, PIXELS_PER_BOHR)

        self.dissect_plan = atom_cloud.subshell_dissection_plan(xs, ys, zs, shells, ells, config)
        self.dissect_ranges = _dissection_ranges(shells, ells, self.dissect_plan)

        print("atom: %s loaded in %dms, scale=%.1f" % (
            slater.element_symbol(z), time.ticks_diff(time.ticks_ms(), t0), self.base_scale))

    def draw_title(self, fb, buf, x, y, text_color):
        _draw_atom_title(fb, buf, x, y, self.z, self.config, text_color)

    def draw_corner_label(self, fb, buf, text_color):
        """Z number, top-right, FONT_SCALE_HUGE, green-yellow -- matches
        src/views/atom_view.cpp's renderAtomFrame() zLabel placement, "so
        the user can see it while browsing the periodic table."
        """
        z_label = str(self.z)
        w = drc.text_width_scaled(z_label, drc.FONT_SCALE_HUGE)
        drc.draw_text_scaled(fb, buf, WIDTH - w, 1, z_label, _GREEN_YELLOW, drc.FONT_SCALE_HUGE)


def run(z=DEFAULT_Z, d=None, detector=None):
    """`d`/`detector` let chooser.py hand this an already-initialized
    display/nudge-detector -- see orbital_view.run()'s matching docstring
    for why. Standalone use (`import atom_view; atom_view.run()`) is
    unaffected, still creates its own.

    Nudging drc.NUDGE_BACK_DIRECTION ('U') returns instead of stepping Z;
    'D' triggers the shell-dissection sequence (see _run_dissection()
    above) instead of stepping -- see device_render_common.py's comment on
    NUDGE_BACK_DIRECTION and _NUDGE_DIRECTION_STEP (which only maps L/R).
    """
    if d is None:
        print("atom: display init...")
        d = display_mod.init()
    print("atom: display ready, Z=1..%d available" % slater.MAX_DISPLAY_Z)

    preset = AtomPresetState(z)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = drc.encode_color565(255, 0, 0)
    text_color = drc.encode_color565(255, 255, 255)
    scale_bar_color = drc.encode_color565(210, 210, 210)

    if detector is None:
        detector = drc.init_nudge_detector("element switching")

    angle = 0.0
    tilt_angle = drc._TILT_ANGLE_START
    roll_angle = drc._ROLL_ANGLE_START
    zoom_angle = 0.0
    two_pi = 2 * math.pi

    angle, tilt_angle, roll_angle = drc.fly_over(
        d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
        preset.base_scale * drc.INTRO_START_SCALE_FACTOR, preset.base_scale, drc.INTRO_FRAMES)

    fps_text = "FPS: --"
    frame_count = 0
    fps_window_start = time.ticks_ms()

    zoom_excursion_countdown = drc.next_zoom_excursion_countdown()
    last_activity_ms = time.ticks_ms()
    # Caps idle auto-advance to at most one dissection per element before it's forced to jump, so
    # idle browsing doesn't get stuck re-dissecting the same element every VIEW_IDLE_JUMP_MS.
    # Reset to False whenever a new element loads, see switch_to_element() below.
    idle_dissected_this_element = False

    def switch_to_element(new_z):
        # Shared by the nudge-driven switch below and the idle auto-jump -- mirrors
        # src/views/atom_view.cpp's runAtomView()'s switchToElement() lambda.
        nonlocal z, preset, angle, tilt_angle, roll_angle, idle_dissected_this_element
        z = new_z
        fb.fill(0)
        fb.text(drc.LOADING_TEXT, drc.LOADING_TEXT_POS[0], drc.LOADING_TEXT_POS[1], text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
        preset = AtomPresetState(z)
        idle_dissected_this_element = False
        angle, tilt_angle, roll_angle = drc.fly_over(
            d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle,
            roll_angle, preset.base_scale * drc.SWITCH_START_SCALE_FACTOR, preset.base_scale,
            drc.SWITCH_TRANSITION_FRAMES)

    while True:
        # Nudge check: steps the atomic number Z and re-does the fly-over on
        # a detected L/R, same as orbital_view.py's preset switch except
        # clamped to [1, MAX_DISPLAY_Z] instead of wrapping -- Z has real
        # endpoints (hydrogen, and the project's Z<=92 display range), unlike
        # a cyclic preset list. Out-of-range nudges are silently ignored. U
        # returns to the menu; D starts the shell-dissection sequence.
        # LOADING_TEXT covers AtomPresetState()'s rebuild so the display
        # doesn't just freeze on the old cloud.
        if detector is not None:
            raw = detector.poll_raw()
            if raw is not None:
                last_activity_ms = time.ticks_ms()
                axis, sign, mag = raw
                direction = detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                if direction == drc.NUDGE_BACK_DIRECTION:
                    print("atom: back nudge -- returning to menu")
                    return
                if direction == 'D':
                    if preset.dissect_plan:
                        print("atom: D nudge -- starting dissection (%d shells)" % len(preset.dissect_plan))
                        angle, tilt_angle, roll_angle = _run_dissection(
                            d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle,
                            roll_angle, detector)
                        zoom_angle = 0.0
                        zoom_excursion_countdown = drc.next_zoom_excursion_countdown()
                    else:
                        print("atom: D nudge -- no subshells to dissect")
                    continue
                step = drc._NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    new_z = z + step
                    if 1 <= new_z <= slater.MAX_DISPLAY_Z:
                        switch_to_element(new_z)

        # Idle auto-advance, MicroPython counterpart of runAtomView()'s idle branch
        # (kViewIdleJumpUs): after 60s with no nudge, either dissect the current element (once
        # per element, coin flip) or jump to a random different one.
        if time.ticks_diff(time.ticks_ms(), last_activity_ms) > drc.VIEW_IDLE_JUMP_MS:
            can_dissect = not idle_dissected_this_element and bool(preset.dissect_plan)
            if can_dissect and random.random() < 0.5:
                print("atom: idle 60s+ -- dissecting current element (Z=%d, %d shells)" % (
                    z, len(preset.dissect_plan)))
                angle, tilt_angle, roll_angle = _run_dissection(
                    d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                    detector)
                idle_dissected_this_element = True
                zoom_angle = 0.0
                zoom_excursion_countdown = drc.next_zoom_excursion_countdown()
            else:
                new_z = drc.random_index_excluding(z - 1, slater.MAX_DISPLAY_Z) + 1
                print("atom: idle 60s+ -- jumping to random element Z=%d" % new_z)
                switch_to_element(new_z)
            last_activity_ms = time.ticks_ms()
            continue

        # Random zoom excursion: pause breathing, fly to a random scale and
        # back (see device_render_common.py's ZOOM_EXCURSION_*). zoom_angle
        # resets to 0 after -- sin(0) == 0 lines up exactly with where the
        # excursion left off. `continue`: this iteration's render already
        # happened inside fly_over(), so skip the normal render/FPS
        # bookkeeping.
        zoom_excursion_countdown -= 1
        if zoom_excursion_countdown <= 0:
            current_scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
            target_scale = preset.base_scale * random.uniform(drc.ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                                drc.ZOOM_EXCURSION_SCALE_MAX_FACTOR)
            angle, tilt_angle, roll_angle = drc.fly_over(
                d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                current_scale, target_scale, drc.ZOOM_EXCURSION_EASE_FRAMES)
            angle, tilt_angle, roll_angle = drc.fly_over(
                d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                target_scale, preset.base_scale, drc.ZOOM_EXCURSION_EASE_FRAMES)
            zoom_angle = 0.0
            zoom_excursion_countdown = drc.next_zoom_excursion_countdown()
            continue

        scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
        drc.render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale)
        preset.draw_title(fb, buf, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], text_color)
        preset.draw_corner_label(fb, buf, text_color)
        fb.text(fps_text, FPS_TEXT_POS[0], FPS_TEXT_POS[1], text_color)
        drc.draw_scale_bar(fb, buf, scale / atom_cloud.PM_PER_BOHR, "pm", scale_bar_color, text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        frame_count += 1
        if frame_count >= FPS_UPDATE_INTERVAL:
            now = time.ticks_ms()
            elapsed_ms = time.ticks_diff(now, fps_window_start)
            fps = 1000.0 * frame_count / elapsed_ms if elapsed_ms > 0 else 0.0
            fps_text = "FPS: %.1f" % fps
            print("atom: %s" % fps_text)
            frame_count = 0
            fps_window_start = now

        angle += drc.ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += drc.TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += drc.ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi
        zoom_angle += drc.ZOOM_ANGLE_STEP
        if zoom_angle >= two_pi:
            zoom_angle -= two_pi
        time.sleep_ms(drc.FRAME_DELAY_MS)


if __name__ == '__main__':
    run()
