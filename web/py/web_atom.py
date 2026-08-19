"""Browser port of pc/atom_view_pc.py's multi-electron atom viewer, built on
web_common.py's canvas/generator backend instead of viewer_common.py's
tkinter/PIL one. See web_common.py's module docstring for why the animation
loop is generator-based here instead of blocking.

AtomPreset and render_dissection_frame() below are copied unchanged from
pc/atom_view_pc.py -- neither ever touched PIL or tkinter (AtomPreset is
pure atom_cloud.py model calls; render_dissection_frame() only ever wrote
into a plain RGB bytearray), so there was nothing to port in them.

What actually changed shape, mirroring web_common.py:
  - _dissect_ease()/_dissect_hold() were blocking loops (root.update() +
    time.sleep() per frame) -- here they're generators
    (dissect_ease_gen()/dissect_hold_gen()), one yield per frame.
    DISSECT_HOLD_SECONDS (a wall-clock pause) becomes DISSECT_HOLD_FRAMES (a
    frame count) for the same reason: there's no blocking sleep to pace
    against once a browser's requestAnimationFrame is driving frames.
  - _run_dissection() is now dissection_sequence(), a generator composing
    the above via `yield from` -- structurally identical to the original,
    just non-blocking.
  - AtomViewApp's tkinter event bindings become plain functions
    (request_z()/request_dissect()/zoom_by(), exposed at module level for
    index.html's JS to call) and its `_tick()` gains one new branch at the
    top: if a generator sequence (intro/switch/dissect/excursion) is
    active, step it and return -- otherwise fall through to the original
    per-frame logic. This replaces AtomViewApp._pending_dissect's "ignore
    while already running" check with the same idea (self.sequence is not
    None) but as the FIRST check every tick makes, since nothing here can
    block anymore.

WebAtomApp is a "scene" class, not a standalone page driver: web_app.py
(the top-level orchestrator behind the chooser screen) constructs one fresh
each time the user picks Element Explorer, and routes tick()/request_z()/
request_dissect()/zoom_by() calls to it while it's the active scene -- see
that module for the chooser and scene-switching logic, and index.html for
how it wires up Pyodide/input.
"""

import math
import time

import atom_cloud
import cloud_common
import slater

import render_core  # shared numpy render core (same module pc/atom_view_pc.py uses)

import web_common as wc
from web_common import (
    WIDTH, HEIGHT, CENTER,
    ANGLE_STEP, TILT_ANGLE_STEP, ROLL_ANGLE_STEP, ZOOM_ANGLE_STEP,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    INTRO_FRAMES, INTRO_START_SCALE_FACTOR,
    SWITCH_TRANSITION_FRAMES, SWITCH_START_SCALE_FACTOR,
    PROTON_SIZE, PROTON_COLOR, ELECTRON_ALPHA, ELECTRON_SIZE,
    TITLE_POS, SUBTITLE_POS, TITLE_FONT_PX,
    render_frame, draw_nucleus, rotate_yaw_tilt_roll, advance_rotation,
    fly_over_gen, zoom_excursion_gen, next_zoom_excursion_countdown,
    outer_bound_scale, inner_bound_scale, shell_count_frames,
    draw_bounding_circle_canvas, draw_orbit_marker_canvas, draw_scale_bar_canvas,
    draw_text_canvas, measure_text_canvas,
)

# --- Cloud / defaults ---------------------------------------------------------
# Trimmed from the PC viewer's 10000 -- Pyodide's interpreted per-point loop,
# not canvas resolution, is the real per-frame cost in a browser tab; 4000 is
# a middle ground between that and the device's 3000.
N_POINTS = 4000
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell

# Calibrated once for THIS canvas's own CENTER -- see
# atom_cloud.pixels_per_bohr_for_canvas()'s docstring. WIDTH/HEIGHT/CENTER
# match pc/viewer_common.py's exactly, so this comes out identical to the PC
# viewer's PIXELS_PER_BOHR.
PIXELS_PER_BOHR = atom_cloud.pixels_per_bohr_for_canvas(CENTER)

# --- Manual zoom (mouse wheel / +- buttons) -----------------------------------
ZOOM_FACTOR_MIN = 0.15
ZOOM_FACTOR_MAX = 8.0
ZOOM_FACTOR_STEP = 1.1

# --- Shell-dissection sequence (see dissection_sequence()) ---------------------
DISSECT_TARGET_PX = 100.0
DISSECT_SHADE_GRAY = (70, 70, 70)
ACTIVE_SUBSHELL_ALPHA = 1.0
DISSECT_CLIP_OPEN = 0.0
DISSECT_CLIP_CLOSED = 1.0e6
DISSECT_ORIENT_FRAMES = 55
DISSECT_ZOOM_FRAMES = 55
# Port of the PC/device dissection pacing change (shell-to-shell hops ~2x as
# long, "every shell-to-shell hop now takes ~2x as long", 2026-08-17) -- only
# the per-shell zoom legs are slowed.
DISSECT_ZOOM_SLOWDOWN = 2.0
DISSECT_HOLD_FRAMES = 100  # ~2s at FRAME_DELAY_MS=5 -- a frame count, not wall-clock seconds
                            # (see module docstring: there's no blocking sleep to pace against)
DISSECT_CLOSE_FRAMES = 100
DISSECT_FRAMES_PER_SHELL = 8

# Device-style dissection HUD (see pc/atom_view_pc.py's matching constants):
# a big subshell label ("2p"), a plain-size caption ("Fe (2/5)", the element symbol) and a
# small "<occ>e-" note in the top-right corner.
DISSECT_BIG_FONT_PX = 72
DISSECT_CAPTION_FONT_PX = 28
DISSECT_OCC_FONT_PX = 24
DISSECT_TITLE_COLOR = (255, 255, 255)
DISSECT_OCC_MARGIN_PX = 8
Z_NOTE_COLOR = (255, 140, 140)


def render_dissection_frame(buf, preset, angle, tilt_angle, roll_angle, scale, clip_z, active_subshell,
                             dim_color=DISSECT_SHADE_GRAY):
    """Same two-pass dim/highlight + clip rendering as pc/atom_view_pc.py's
    render_dissection_frame() -- see that module for the full docstring. With
    numpy (always present in Pyodide) this takes the SHARED vectorized core
    (render_core.render_dissection_frame_np, the same function the PC uses);
    the pure-Python loop below is the no-numpy fallback.
    """
    if render_core._HAS_NUMPY and ELECTRON_SIZE in (1, 2):
        arr = render_core.preset_np(preset)
        if arr is not None:
            render_core.render_dissection_frame_np(
                buf, preset, arr, angle, tilt_angle, roll_angle, scale,
                clip_z, active_subshell, dim_color,
                WIDTH, HEIGHT, CENTER, ELECTRON_SIZE, ELECTRON_ALPHA, ACTIVE_SUBSHELL_ALPHA,
                PROTON_SIZE, PROTON_COLOR)
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
                continue

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
                    n = shells[i]
                    cr, cg, cb = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
                else:
                    cr, cg, cb = colors[i]
                buf[idx] = buf[idx] + int((cr - buf[idx]) * alpha)
                buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * alpha)
                buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * alpha)

    _draw(only_subshell=None, dim=(active_subshell is not None), alpha=ELECTRON_ALPHA)
    if active_subshell is not None:
        _draw(only_subshell=active_subshell, dim=False, alpha=ACTIVE_SUBSHELL_ALPHA)

    draw_nucleus(buf)


def draw_atom_title_canvas(x, y, z, config):
    """Canvas counterpart of pc/atom_view_pc.py's draw_atom_title(): same
    per-shell-colored electron-configuration label, drawn segment by segment
    since Canvas 2D's fillText() is single-color per call just like PIL's
    ImageDraw.text(), advancing x by each segment's measured width.
    """
    prefix = "%s (Z=%d) " % (slater.element_symbol(z), z)
    draw_text_canvas(x, y, prefix, (255, 255, 255))
    cursor_x = x + measure_text_canvas(prefix)
    for n, ell, occ in config:
        segment = "%s%d " % (slater.subshell_label(n, ell), occ)
        color = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
        draw_text_canvas(cursor_x, y, segment, color)
        cursor_x += measure_text_canvas(segment)


class AtomPreset:
    """Unchanged from pc/atom_view_pc.py's AtomPreset -- pure atom_cloud.py
    model calls, no PIL/tkinter dependency to begin with.
    """

    def __init__(self, z):
        print("atom: loading Z=%d (%s)..." % (z, slater.element_symbol(z)))
        t0 = time.time()

        xs, ys, zs, colors, shells, ells, signs, config = atom_cloud.build_atom_point_cloud(z, count=N_POINTS)

        self.xs, self.ys, self.zs, self.colors, self.shells, self.ells, self.signs, self.config = (
            xs, ys, zs, colors, shells, ells, signs, config)
        self.title = atom_cloud.title_for_atom(z, config)
        outer_plan = atom_cloud.subshell_dissection_plan(xs, ys, zs, shells, ells, config)
        r_ref = outer_plan[0][5] if outer_plan else 1.0
        self.base_scale, self.zoom_amplitude, self.r_ref = atom_cloud.scale_for_atom(
            r_ref, PIXELS_PER_BOHR)
        self.inner_r_ref = outer_plan[-1][5] if outer_plan else r_ref
        self.shell_count = len(outer_plan) if outer_plan else 1

        print("atom: %s loaded in %.2fs, scale=%.1f" % (
            slater.element_symbol(z), time.time() - t0, self.base_scale))

    def resample(self, count):
        pass  # static cloud -- see atom_cloud.py's module docstring


class WebAtomApp:
    """Generator-driven counterpart of pc/atom_view_pc.py's AtomViewApp --
    see module docstring for the shape change from tkinter's blocking loop.
    """

    def __init__(self):
        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.z = DEFAULT_Z
        self.preset = AtomPreset(self.z)
        self.pending_z = None
        self.zoom_factor = 1.0
        self.dissecting = False
        self.pending_dissect = False

        self.angle = 0.0
        self.tilt_angle = _TILT_ANGLE_START
        self.roll_angle = _ROLL_ANGLE_START
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi
        self.zoom_excursion_countdown = next_zoom_excursion_countdown()

        self.sequence = None  # active generator (intro/switch/dissect/excursion), if any

    def effective_base_scale(self):
        return self.preset.base_scale * self.zoom_factor

    def effective_zoom_amplitude(self):
        return self.preset.zoom_amplitude * self.zoom_factor

    def blit(self, scale):
        wc.blit_buf(self.buf)
        draw_orbit_marker_canvas(self.preset.r_ref, scale, self.angle, self.tilt_angle, self.roll_angle,
                                  slater.element_symbol(self.z))
        draw_scale_bar_canvas(cloud_common, scale / atom_cloud.PM_PER_BOHR, "pm")
        draw_atom_title_canvas(TITLE_POS[0], TITLE_POS[1], self.z, self.preset.config)

    def blit_dissection(self, scale, r_ref, title):
        """Device-style dissection HUD (same as pc/atom_view_pc.py's
        _blit_dissection): plain gray bounding circle, scale bar, a big
        subshell label ("2p") with a plain-size caption ("Fe (2/5)", the
        element symbol) underneath, a small "<occ>e-" note in the top-right corner, and the
        red Z note by the nucleus. `title` is a (big_label, caption, occ)
        tuple or None.
        """
        wc.blit_buf(self.buf)
        draw_bounding_circle_canvas(r_ref, scale)
        draw_scale_bar_canvas(cloud_common, scale / atom_cloud.PM_PER_BOHR, "pm")
        if title is not None:
            big_label, caption, occ = title
            draw_text_canvas(TITLE_POS[0], TITLE_POS[1], big_label, DISSECT_TITLE_COLOR,
                             font_px=DISSECT_BIG_FONT_PX)
            draw_text_canvas(TITLE_POS[0], TITLE_POS[1] + DISSECT_BIG_FONT_PX + 6, caption,
                             DISSECT_TITLE_COLOR, font_px=DISSECT_CAPTION_FONT_PX)
            occ_text = "%de-" % occ
            occ_x = WIDTH - measure_text_canvas(occ_text, font_px=DISSECT_OCC_FONT_PX) - DISSECT_OCC_MARGIN_PX
            draw_text_canvas(occ_x, DISSECT_OCC_MARGIN_PX, occ_text, DISSECT_TITLE_COLOR,
                             font_px=DISSECT_OCC_FONT_PX)
        draw_text_canvas(CENTER + PROTON_SIZE, CENTER - PROTON_SIZE, "Z=%d" % self.z, Z_NOTE_COLOR)

    def dissect_tumble(self):
        self.roll_angle = (self.roll_angle + ROLL_ANGLE_STEP) % self.two_pi

    def dissect_ease_gen(self, scale0, scale1, clip0, clip1, active_subshell, r_ref, frames, title=None,
                          full_tumble=False):
        """See pc/atom_view_pc.py's _dissect_ease() docstring for
        full_tumble's meaning -- same "clip is CLOSED throughout this leg,
        so full 3-axis tumble is safe" reasoning, used the same way by
        dissection_sequence()'s Phase 0/5. `title` is the (big_label,
        caption, occ) triple for blit_dissection(), or None.
        """
        for i in range(frames):
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = scale0 + (scale1 - scale0) * t
            clip = clip0 + (clip1 - clip0) * t
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_subshell)
            self.blit_dissection(scale, r_ref, title)
            if full_tumble:
                advance_rotation(self)
            else:
                self.dissect_tumble()
            yield

    def dissect_hold_gen(self, scale, clip, active_subshell, r_ref, frames, title):
        for _ in range(frames):
            render_dissection_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle,
                                     scale, clip, active_subshell)
            self.blit_dissection(scale, r_ref, title)
            self.dissect_tumble()
            yield

    def dissection_sequence(self):
        """Structurally identical to pc/atom_view_pc.py's _run_dissection()
        -- see that method's docstring for the phase-by-phase rationale.
        """
        plan = atom_cloud.subshell_dissection_plan(
            self.preset.xs, self.preset.ys, self.preset.zs, self.preset.shells, self.preset.ells,
            self.preset.config)

        shell_count = self.preset.shell_count
        orient_frames = shell_count_frames(DISSECT_ORIENT_FRAMES, DISSECT_FRAMES_PER_SHELL, shell_count)
        # Shell-to-shell hops paced ~2x slower (see DISSECT_ZOOM_SLOWDOWN).
        zoom_frames = int(shell_count_frames(DISSECT_ZOOM_FRAMES, DISSECT_FRAMES_PER_SHELL, shell_count)
                          * DISSECT_ZOOM_SLOWDOWN)
        close_frames = shell_count_frames(DISSECT_CLOSE_FRAMES, DISSECT_FRAMES_PER_SHELL, shell_count)

        resting_scale = self.effective_base_scale() + self.effective_zoom_amplitude() * math.sin(self.zoom_angle)
        outer_scale = outer_bound_scale(self.preset.r_ref)
        inner_scale = inner_bound_scale(self.preset.inner_r_ref)

        # Phase 0: ease out to the guaranteed "outside" overview, cut closed.
        # full_tumble=True: the cut has no effect yet (clip stays closed
        # this whole leg), so yaw/tilt can keep rotating normally instead of
        # locking to roll-only the instant Dissect is pressed -- see
        # dissect_ease_gen()'s docstring.
        yield from self.dissect_ease_gen(resting_scale, outer_scale, DISSECT_CLIP_CLOSED, DISSECT_CLIP_CLOSED,
                                          None, self.preset.r_ref, zoom_frames, full_tumble=True)
        # Phase 1: open the cut at that overview scale.
        yield from self.dissect_ease_gen(outer_scale, outer_scale, DISSECT_CLIP_CLOSED, DISSECT_CLIP_OPEN,
                                          None, self.preset.r_ref, orient_frames)

        # Phase 2: outermost subshell to innermost; the last one is pinned to
        # inner_scale (deeper than its own radius), the rest fill DISSECT_TARGET_PX.
        # Title is the device-style triple: big "2p" + "Fe (k/N)" caption
        # (element symbol, not the shell notation already shown by the big
        # label) + corner "<occ>e-" note (see blit_dissection()).
        prev_scale = outer_scale
        for i, (n, ell, letter, subshell_str, electron_count, r_ref) in enumerate(plan):
            target_scale = inner_scale if i == len(plan) - 1 else DISSECT_TARGET_PX / max(r_ref, 1e-6)
            subshell = slater.subshell_label(n, ell)
            title = (subshell, "%s (%d/%d)" % (slater.element_symbol(self.z), i + 1, len(plan)), electron_count)
            yield from self.dissect_ease_gen(prev_scale, target_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_OPEN,
                                              (n, ell), r_ref, zoom_frames, title)
            yield from self.dissect_hold_gen(target_scale, DISSECT_CLIP_OPEN, (n, ell), r_ref,
                                              DISSECT_HOLD_FRAMES, title)
            prev_scale = target_scale

        # Phase 3: back out to the overview scale, still open.
        yield from self.dissect_ease_gen(prev_scale, outer_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_OPEN,
                                          None, self.preset.r_ref, zoom_frames)
        # Phase 4: close the cut at the overview scale.
        yield from self.dissect_ease_gen(outer_scale, outer_scale, DISSECT_CLIP_OPEN, DISSECT_CLIP_CLOSED,
                                          None, self.preset.r_ref, close_frames)
        # Phase 5: ease back in to the SAME resting_scale Phase 0 started
        # from (not effective_base_scale(), which omits the breathing sin()
        # term Phase 0's start point included -- see pc/atom_view_pc.py's
        # matching comment), cut already closed. full_tumble=True for the
        # same reason as Phase 0.
        yield from self.dissect_ease_gen(outer_scale, resting_scale,
                                          DISSECT_CLIP_CLOSED, DISSECT_CLIP_CLOSED,
                                          None, self.preset.r_ref, zoom_frames, full_tumble=True)

    def dissection_wrapper(self):
        self.dissecting = True
        try:
            yield from self.dissection_sequence()
        finally:
            self.dissecting = False

    def request_z(self, step):
        new_z = self.z + step
        if 1 <= new_z <= slater.MAX_DISPLAY_Z:
            self.pending_z = new_z

    def request_dissect(self):
        if not self.dissecting:
            self.pending_dissect = True

    def zoom_by(self, factor):
        self.zoom_factor = min(ZOOM_FACTOR_MAX, max(ZOOM_FACTOR_MIN, self.zoom_factor * factor))

    def start(self):
        """Kicks off the intro fly-over. Does NOT bind the canvas -- that
        happens once, globally, in web_app.py's WebApp.start(); this app
        (like WebOrbitalApp) is constructed fresh each time the user picks
        Element Explorer from the chooser, but they all share the one
        canvas web_app.py already bound.
        """
        self.sequence = fly_over_gen(self, self.effective_base_scale() * INTRO_START_SCALE_FACTOR,
                                      self.effective_base_scale(), INTRO_FRAMES)

    def tick(self):
        if self.sequence is not None:
            try:
                next(self.sequence)
            except StopIteration:
                self.sequence = None
            return

        if self.pending_dissect:
            self.pending_dissect = False
            self.sequence = self.dissection_wrapper()
            return

        if self.pending_z is not None:
            self.z = self.pending_z
            self.pending_z = None
            self.preset = AtomPreset(self.z)
            self.sequence = fly_over_gen(self, self.effective_base_scale() * SWITCH_START_SCALE_FACTOR,
                                          self.effective_base_scale(), SWITCH_TRANSITION_FRAMES)
            return

        self.zoom_excursion_countdown -= 1
        if self.zoom_excursion_countdown <= 0:
            self.sequence = zoom_excursion_gen(self, self.effective_base_scale(), self.effective_zoom_amplitude(),
                                                self.preset.r_ref, self.preset.inner_r_ref,
                                                shell_count=self.preset.shell_count, scale_factor=self.zoom_factor)
            return

        scale = self.effective_base_scale() + self.effective_zoom_amplitude() * math.sin(self.zoom_angle)
        render_frame(self.buf, self.preset, self.angle, self.tilt_angle, self.roll_angle, scale)
        self.blit(scale)

        advance_rotation(self)
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi


