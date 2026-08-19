"""Atom-dissection logic SHARED by pc/atom_view_pc.py and web/py/web_atom.py
(fetched into Pyodide the same way pc/render_core.py already is -- see
web/index.html's PY_FILES). No PIL/tkinter/canvas here: AtomPreset is pure
atom_cloud.py model calls, and build_dissection_steps() is pure phase-plan
arithmetic -- neither platform's rendering primitives leak in here, matching
render_core.py's split (render math shared, blit/input stays platform-side).

This module does NOT cover micropython/atom_view.py: the device has no
dissection feature (see that module's own docstring), so there is nothing
to share with it here.

Why build_dissection_steps() exists: pc/atom_view_pc.py's old
_run_dissection() and web/py/web_atom.py's old dissection_sequence() computed
the Phase 0-5 scale/clip/timing plan with genuinely duplicated arithmetic --
proven real (not just superficial similarity) by the 2026-08-17 pacing tweak
(DISSECT_ZOOM_SLOWDOWN) needing a hand-port across pc, web, AND the C++
device. This function computes that plan ONCE as a list of opaque step
tuples; each platform just executes the steps with its own ease/hold
primitive (pc: blocking _dissect_ease()/_dissect_hold(); web: generators
dissect_ease_gen()/dissect_hold_gen()) and its own hold-duration units (pc:
wall-clock seconds; web: a frame count -- hold_duration is never interpreted
here, just threaded through unchanged).
"""

import array
import time

import atom_cloud
import slater

# Step shape:
#   ('ease', scale0, scale1, clip0, clip1, active_subshell, r_ref, frames, title, full_tumble)
#   ('hold', scale, clip, active_subshell, r_ref, hold_duration, title)
# title is (big_label, caption, electron_count) or None -- see
# build_dissection_steps()'s docstring.


def default_size_factor(z):
    """Hydrogenic-model Clementi-Raimondi calibration factor
    (atom_size_calib.py, generated -- shared with the micropython and C++
    ports) -- the size factor every platform uses except pc's optional
    radial_tables (HFS) mode, which computes its own instead (see
    pc/atom_view_pc.py's clementi_size_factor()). 1.0 (no rescale) for
    Z outside the table (Z > slater.MAX_DISPLAY_Z) or if the generated
    table is missing.
    """
    try:
        import atom_size_calib
    except ImportError:
        return 1.0
    if 1 <= z <= len(atom_size_calib.FACTOR):
        return atom_size_calib.FACTOR[z - 1]
    return 1.0


class AtomPreset:
    """One atomic number Z's point cloud plus the derived scale/zoom/legend
    state every platform's tumbling-camera loop needs -- same public shape
    as an orbital Preset (xs/ys/zs/colors/title/base_scale/zoom_amplitude/
    r_ref/resample()) so each platform's shared render_frame() accepts it
    unchanged, plus shells/ells/signs/config/outer_n/outer_ell for the
    dissection view and the shell-colored legend (draw_atom_title()/
    draw_atom_title_canvas()).

    size_factor rescales the whole cloud (see default_size_factor()'s
    docstring on why this exists) so the valence subshell's mode radius
    lands on the Clementi-Raimondi literature value; pass 1.0 for no
    rescale. Scaling preserves signs (R(r/f) keeps R(r)'s node structure)
    and the internal relative shell structure -- only the atom's overall
    size changes.
    """

    def __init__(self, z, count, pixels_per_bohr, size_factor=1.0, radial_tables=None,
                 log_prefix="atom"):
        print("%s: loading Z=%d (%s)..." % (log_prefix, z, slater.element_symbol(z)))
        t0 = time.time()

        xs, ys, zs, colors, shells, ells, signs, config = atom_cloud.build_atom_point_cloud(
            z, count=count, radial_tables=radial_tables)

        if size_factor != 1.0:
            xs = array.array('f', (v * size_factor for v in xs))
            ys = array.array('f', (v * size_factor for v in ys))
            zs = array.array('f', (v * size_factor for v in zs))

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
            r_ref, pixels_per_bohr)
        # Which (n, ell) is the outermost subshell BY MEASURED RADIUS (not
        # just the last entry in `config`'s Madelung-order list -- see the
        # 4s/3d crossover note in subshell_dissection_plan()) -- draw_atom_title()
        # brightens this one subshell's config-text segment the same way its
        # points are brightened, so the legend and the cloud read as one
        # language.
        self.outer_n, self.outer_ell = (outer_plan[0][0], outer_plan[0][1]) if outer_plan else (0, 0)
        # Innermost/first shell's own radius and the subshell count -- used
        # by the shared zoom envelope (outer_bound_scale()/inner_bound_scale()/
        # shell_count_frames()) to guarantee dives/dissections always reach
        # the first shell's own depth and to pace their duration by how many
        # subshells this element actually has.
        self.inner_r_ref = outer_plan[-1][5] if outer_plan else r_ref
        self.shell_count = len(outer_plan) if outer_plan else 1
        self._np_cache = None  # numpy fast-path arrays; rebuilt lazily (see render_core.preset_np)

        print("%s: %s loaded in %.2fs, scale=%.1f" % (
            log_prefix, slater.element_symbol(z), time.time() - t0, self.base_scale))

    def resample(self, count):
        pass  # static cloud -- a mixture of several subshells, unlike cloud_common's single-orbital turnover


def dissection_plan(preset):
    """The current preset's subshell dissection plan (see
    atom_cloud.subshell_dissection_plan()) -- shared by both platforms'
    dissection entry point and idle auto-advance's can-dissect check.
    """
    return atom_cloud.subshell_dissection_plan(
        preset.xs, preset.ys, preset.zs, preset.shells, preset.ells, preset.config)


def build_dissection_steps(plan, r_ref, resting_scale, outer_scale, inner_scale,
                            orient_frames, zoom_frames, close_frames, hold_duration,
                            target_px, clip_open, clip_closed, element_symbol):
    """The full Phase 0-5 shell-dissection sequence as a flat list of opaque
    step tuples (see module docstring for the two shapes) -- same plan
    pc/atom_view_pc.py's _run_dissection() and web/py/web_atom.py's
    dissection_sequence() used to compute by hand, in lockstep, inline.

    Phase 0: ease from resting_scale out to outer_scale (the guaranteed
      "outside" overview), cut closed, full_tumble (nothing is cut yet, so
      yaw/tilt can keep advancing normally instead of locking to roll-only
      the instant the sequence starts).
    Phase 1: open the cut at outer_scale, no subshell singled out yet.
    Phase 2: outermost subshell to innermost -- ease to each subshell's own
      DISSECT_TARGET_PX-filling scale (the last/innermost one pinned to
      inner_scale instead, guaranteeing the dive reaches
      ZOOM_INNER_RADIUS_FACTOR x its radius, not just its own radius), hold
      with its label shown.
    Phase 3: back out to outer_scale, still open, nothing singled out.
    Phase 4: close the cut at outer_scale.
    Phase 5: ease back in to resting_scale, cut closed, full_tumble (same
      reasoning as Phase 0) -- so normal viewing's next frame resumes
      exactly where this sequence leaves off.

    title = (subshell_label, "<Sym> (i/N)", electron_count) for Phase 2's
    ease+hold pair, None everywhere else -- built here so callers don't
    duplicate that format string either.
    """
    steps = []
    steps.append(('ease', resting_scale, outer_scale, clip_closed, clip_closed,
                  None, r_ref, zoom_frames, None, True))
    steps.append(('ease', outer_scale, outer_scale, clip_closed, clip_open,
                  None, r_ref, orient_frames, None, False))

    prev_scale = outer_scale
    for i, (n, ell, _letter, _subshell_str, electron_count, sub_r_ref) in enumerate(plan):
        target_scale = inner_scale if i == len(plan) - 1 else target_px / max(sub_r_ref, 1e-6)
        subshell_label = slater.subshell_label(n, ell)
        title = (subshell_label, "%s (%d/%d)" % (element_symbol, i + 1, len(plan)), electron_count)
        steps.append(('ease', prev_scale, target_scale, clip_open, clip_open,
                      (n, ell), sub_r_ref, zoom_frames, title, False))
        steps.append(('hold', target_scale, clip_open, (n, ell), sub_r_ref, hold_duration, title))
        prev_scale = target_scale

    steps.append(('ease', prev_scale, outer_scale, clip_open, clip_open,
                  None, r_ref, zoom_frames, None, False))
    steps.append(('ease', outer_scale, outer_scale, clip_open, clip_closed,
                  None, r_ref, close_frames, None, False))
    steps.append(('ease', outer_scale, resting_scale, clip_closed, clip_closed,
                  None, r_ref, zoom_frames, None, True))
    return steps
