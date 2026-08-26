"""Standalone rendering-performance benchmark, MicroPython counterpart of
src/debug/benchmark_test.cpp: builds the SAME fixed atom (Fe, Z=26) and the
SAME fixed orbital preset (2p_z, cloud_common.ORBITAL_PRESETS[DEFAULT_PRESET_INDEX]
-- index-matched to src/physics/orbital_library.h's kOrbitalDefaultPresetIndex)
at the SAME point counts (500/1000/2000/4000/8000) as the C++ sweep, timing
build + real-render-path frames at each size, so the two platforms' numbers
are directly comparable -- see BENCHMARK.md for the C++ side and how to read
the results.

Uses atom_view.py's own radial model (HFS/atomSFE tables, hfs_tables.bin) for
the atom sweep, matching what src/physics/atom_cloud.cpp uses on-device
(data/hfs_tables.bin) rather than the plain hydrogenic Z_eff fallback --
same reasoning as atom_view.py's own _HFS_TABLES load.

Not wired into chooser.py/main.py -- run standalone the same way
atom_view.py/orbital_view.py are, after copying micropython/. to the board
(see main.py's docstring for the mpremote incantation):
    mpremote connect <port> exec "import benchmark_test; benchmark_test.run()"

Timing model differs from the C++ side in one way worth noting: display.py's
ST7789.blit_buffer() is a synchronous blocking SPI write (see st7789py.py),
not a DMA-kickoff-then-wait-later split like Display::presentFrame()/
waitForFlushDone() -- so there is no separate "wait for previous frame's DMA"
phase to time here. `compute_ms` below is CPU-only (render_frame() minus the
blit); `blit_ms` is the SPI transfer alone; `frame_ms` = their sum, the
MicroPython analogue of the C++ side's avg_render_ms + avg_wait_ms combined.
"""

import gc
import math
import time

import array

import atom_cloud
import cloud_common
import device_render_common as drc
import display as display_mod
import hfs_radial_tables
import slater

WIDTH = drc.WIDTH
HEIGHT = drc.HEIGHT
CENTER = drc.CENTER

BENCH_ATOMIC_NUMBER = 26  # Fe -- matches src/debug/benchmark_test.cpp's kBenchAtomicNumber
BENCH_ORBITAL_PRESET_INDEX = cloud_common.DEFAULT_PRESET_INDEX  # 2p_z, matches kOrbitalDefaultPresetIndex
BENCH_POINT_COUNTS = (500, 1000, 2000, 4000, 8000)  # matches kBenchPointCounts
BENCH_SEED = cloud_common.SEED  # 12345, matches kAtomCloudSeed / atom_cloud.SEED
BENCH_FRAMES_PER_STEP = 30  # half the C++ side's 60 -- MicroPython's per-point sampling (build,
                            # not render -- render_points() is viper, comparable speed) is far
                            # slower than the C++ build, so the full 5x8000pt x2-sweep x60-frame
                            # schedule would take many extra minutes for a metric (avg fps) that
                            # already converges well below 60 samples. build_ms itself is
                            # unaffected by this constant.

PROTON_COLOR = drc.encode_color565(255, 0, 0)
TEXT_COLOR = drc.encode_color565(255, 255, 255)
SCALE_BAR_COLOR = drc.encode_color565(210, 210, 210)


class _BenchPreset:
    """Minimal stand-in for AtomPresetState/orbital_view.PresetState: just
    the fields device_render_common.render_frame() actually reads
    (xs_fx/ys_fx/zs_fx/colors) plus a draw_title() method, built directly at
    an explicit `count` rather than each app's own hardcoded N_POINTS --
    neither AtomPresetState nor orbital_view.PresetState take a count
    override, so this sweep can't reuse them as-is.
    """

    def __init__(self, xs, ys, zs, colors, title_fn):
        """`colors` is already an encoded array('H') of RGB565 values (see
        device_render_common.encode_orbital_colors()/encode_rgb_colors()) --
        callers do their own (native-compiled) encoding before constructing
        this, same division of labor as AtomPresetState/PresetState.
        """
        self.xs_fx = drc.to_fixed(xs)
        self.ys_fx = drc.to_fixed(ys)
        self.zs_fx = drc.to_fixed(zs)
        self.colors = colors
        self._title_fn = title_fn

    def draw_title(self, fb, buf, x, y, text_color):
        self._title_fn(fb, x, y, text_color)

    def draw_corner_label(self, fb, buf, text_color):
        pass  # benchmark keeps the simpler single-line title C++'s own benchmark uses
              # (kFontLarge "Fe (Z=26)", not the live view's split symbol+corner-Z layout) --
              # see src/debug/benchmark_test.cpp's runAtomStep()/runOrbitalStep()


def _outer_subshell_info(xs, ys, zs, shells, ells, config):
    """(n, ell, r_ref) of the outermost occupied subshell -- MicroPython
    counterpart of the C++ side's OuterSubshell/outerSubshellRRef(), reusing
    atom_cloud.subshell_dissection_plan() (already sorts by measured p90
    radius, descending -- plan[0] is the outermost).
    """
    plan = atom_cloud.subshell_dissection_plan(xs, ys, zs, shells, ells, config)
    if not plan:
        return 0, 0, 1.0
    n, ell, _letter, _label, _occ, r_ref = plan[0]
    return n, ell, r_ref


def _time_frames(fb, buf, d, preset, angle, tilt_angle, roll_angle, scale, frames, buzz_threshold=0):
    """Render+blit `frames` frames through the real production pipeline
    (render_frame() -> proton marker + render_points() [viper Q8] + title +
    scale bar, then blit_buffer()), timing compute (everything up to and
    including drawing, excluding the blit) and blit (the SPI transfer)
    separately -- see module docstring for why there's no separate "wait"
    phase here. Angle/tilt/roll advance each frame the same way the real
    viewers' steady-state loop does (device_render_common.ANGLE_STEP etc.),
    so later frames aren't all identical (matters for buzz_threshold != 0).
    """
    two_pi = 2 * math.pi
    compute_accum = 0
    blit_accum = 0
    min_frame_ms = -1.0
    max_frame_ms = 0.0

    for f in range(frames):
        t0 = time.ticks_us()
        drc.render_frame(fb, buf, preset, PROTON_COLOR, angle, tilt_angle, roll_angle, scale, f, buzz_threshold)
        preset.draw_title(fb, buf, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], TEXT_COLOR)
        drc.draw_scale_bar(fb, buf, scale / cloud_common.PM_PER_BOHR, "pm", SCALE_BAR_COLOR, TEXT_COLOR)
        t1 = time.ticks_us()
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
        t2 = time.ticks_us()

        compute_ms = time.ticks_diff(t1, t0) / 1000.0
        blit_ms = time.ticks_diff(t2, t1) / 1000.0
        frame_ms = compute_ms + blit_ms
        compute_accum += compute_ms
        blit_accum += blit_ms
        if min_frame_ms < 0 or frame_ms < min_frame_ms:
            min_frame_ms = frame_ms
        if frame_ms > max_frame_ms:
            max_frame_ms = frame_ms

        angle += drc.ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += drc.TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += drc.ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi

    avg_compute_ms = compute_accum / frames
    avg_blit_ms = blit_accum / frames
    avg_frame_ms = avg_compute_ms + avg_blit_ms
    fps = 1000.0 / avg_frame_ms if avg_frame_ms > 0 else 0.0
    return avg_compute_ms, avg_blit_ms, avg_frame_ms, min_frame_ms, max_frame_ms, fps


def _run_atom_step(fb, buf, d, count, radial_tables, pixels_per_bohr, angle, tilt_angle, roll_angle):
    t0 = time.ticks_ms()
    xs, ys, zs, colors_rgb, shells, ells, _signs, config = atom_cloud.build_atom_point_cloud(
        BENCH_ATOMIC_NUMBER, count=count, seed=BENCH_SEED, radial_tables=radial_tables)
    colors = drc.encode_rgb_colors(colors_rgb)  # matches C++'s buildMs boundary, which
                                                 # includes colorizeAtomSubshells() too
    build_ms = time.ticks_diff(time.ticks_ms(), t0)

    outer_n, outer_ell, r_ref = _outer_subshell_info(xs, ys, zs, shells, ells, config)
    base_scale, _zoom_amp, _r_ref = atom_cloud.scale_for_atom(r_ref, pixels_per_bohr)

    symbol = slater.element_symbol(BENCH_ATOMIC_NUMBER)

    def title_fn(fb_, x, y, text_color):
        cursor_x = x
        cursor_y = y
        seg = "%s (Z=%d) " % (symbol, BENCH_ATOMIC_NUMBER)
        fb_.text(seg, cursor_x, cursor_y, text_color)
        cursor_x += len(seg) * 8
        for n, ell, occ in config:
            seg = "%s%d " % (slater.subshell_label(n, ell), occ)
            r, g, b = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
            color = drc.encode_color565(r, g, b)
            seg_w = len(seg) * 8
            if cursor_x > x and cursor_x + seg_w > WIDTH:
                cursor_x = x
                cursor_y += 10
            fb_.text(seg, cursor_x, cursor_y, color)
            cursor_x += seg_w

    preset = _BenchPreset(xs, ys, zs, colors, title_fn)
    avg_compute_ms, avg_blit_ms, avg_frame_ms, min_ms, max_ms, fps = _time_frames(
        fb, buf, d, preset, angle, tilt_angle, roll_angle, base_scale, BENCH_FRAMES_PER_STEP)

    gc.collect()
    heap_free = gc.mem_free()
    print("BENCH,STEP,kind,atom,points,%d,build_ms,%d,avg_compute_ms,%.3f,avg_blit_ms,%.3f,avg_frame_ms,%.3f,"
          "min_frame_ms,%.3f,max_frame_ms,%.3f,fps,%.2f,heap_free,%d" % (
              count, build_ms, avg_compute_ms, avg_blit_ms, avg_frame_ms, min_ms, max_ms, fps, heap_free))
    print("BENCH,GEOM,points,%d,outer_n,%d,outer_ell,%d,outer_rref_bohr,%.6f,base_scale_px,%.6f" % (
        count, outer_n, outer_ell, r_ref, base_scale))


def _run_orbital_step(fb, buf, d, count, angle, tilt_angle, roll_angle):
    n, ell, m, label = cloud_common.ORBITAL_PRESETS[BENCH_ORBITAL_PRESET_INDEX]
    phase_pair = cloud_common.ORBITAL_PHASE_COLORS[BENCH_ORBITAL_PRESET_INDEX]
    title = cloud_common.title_for_preset(cloud_common.ORBITAL_PRESETS[BENCH_ORBITAL_PRESET_INDEX])

    t0 = time.ticks_ms()
    xs, ys, zs, psi2, signs, _sampler, _rng, _radial_coeff, _legendre_coeff = cloud_common.build_point_cloud(
        n, ell, m, count=count, seed=BENCH_SEED)
    levels, _psi2_sorted = cloud_common.compute_levels(psi2)
    colors = drc.encode_orbital_colors(levels, signs, phase_pair)
    build_ms = time.ticks_diff(time.ticks_ms(), t0)

    base_scale, _zoom_amp, _r_ref = cloud_common.scale_from_radii(xs, ys, zs)

    def title_fn(fb_, x, y, text_color):
        fb_.text(title, x, y, text_color)

    preset = _BenchPreset(xs, ys, zs, colors, title_fn)
    avg_compute_ms, avg_blit_ms, avg_frame_ms, min_ms, max_ms, fps = _time_frames(
        fb, buf, d, preset, angle, tilt_angle, roll_angle, base_scale, BENCH_FRAMES_PER_STEP)

    gc.collect()
    heap_free = gc.mem_free()
    print("BENCH,STEP,kind,orbital,points,%d,build_ms,%d,avg_compute_ms,%.3f,avg_blit_ms,%.3f,avg_frame_ms,%.3f,"
          "min_frame_ms,%.3f,max_frame_ms,%.3f,fps,%.2f,heap_free,%d" % (
              count, build_ms, avg_compute_ms, avg_blit_ms, avg_frame_ms, min_ms, max_ms, fps, heap_free))


def run(d=None):
    if d is None:
        print("benchmark: display init...")
        d = display_mod.init()

    buf = bytearray(WIDTH * HEIGHT * 2)
    import framebuf
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    gc.collect()
    print("BENCH,MEM,start,heap_free,%d" % gc.mem_free())

    symbol = slater.element_symbol(BENCH_ATOMIC_NUMBER)
    _n, _ell, _m, orbital_label = cloud_common.ORBITAL_PRESETS[BENCH_ORBITAL_PRESET_INDEX]
    print("BENCH,START,atom,%s,Z,%d,orbital,%s,frames_per_step,%d" % (
        symbol, BENCH_ATOMIC_NUMBER, orbital_label, BENCH_FRAMES_PER_STEP))

    # Correctness fingerprint, part 1 (see src/debug/benchmark_test.cpp's matching block):
    # electron configuration + per-subshell Z_eff are pure functions of Z, directly comparable
    # against the C++ side's BENCH,CONFIG/BENCH,ZEFF lines for the same element.
    config = slater.electron_configuration(BENCH_ATOMIC_NUMBER)
    for n, ell, occ in config:
        print("BENCH,CONFIG,%s,%d,%d,%d" % (symbol, n, ell, occ))
    for n, ell, occ in config:
        z_eff = slater.z_eff_radial(BENCH_ATOMIC_NUMBER, config, n, ell)
        print("BENCH,ZEFF,%s,%d,%d,%.17g" % (symbol, n, ell, z_eff))

    print("benchmark: loading HFS radial tables...")
    radial_tables = hfs_radial_tables.load()
    pixels_per_bohr = atom_cloud.pixels_per_bohr_for_canvas(CENTER)

    angle = 0.0
    tilt_angle = drc._TILT_ANGLE_START
    roll_angle = drc._ROLL_ANGLE_START

    # Warm atom_cloud.py's per-subshell table caches (_RADIAL_TABLE_CACHE/_ANISO_SAMPLER_CACHE)
    # for Fe BEFORE any timed sweep step, same as pixels_per_bohr_for_canvas() above already did
    # incidentally for Rb (a different element, so it doesn't warm Fe's own entries). This makes
    # every timed atom step below measure ONLY point-sampling + outer-shell coloring -- the
    # genuinely-comparable-to-C++ cost -- instead of the first step alone silently absorbing the
    # one-time table-build cost C++ never pays at runtime (its equivalent angular tables are
    # compile-time-embedded, see atom_cloud.py's cache comment). That one-time cost is real and
    # reported on its own line (BENCH,TABLEBUILD) rather than hidden.
    print("benchmark: warming atom point-cloud caches (Fe subshells)...")
    t0 = time.ticks_ms()
    atom_cloud.build_atom_point_cloud(BENCH_ATOMIC_NUMBER, count=BENCH_POINT_COUNTS[0], seed=BENCH_SEED,
                                      radial_tables=radial_tables)
    print("BENCH,TABLEBUILD,kind,atom,warm_ms,%d" % time.ticks_diff(time.ticks_ms(), t0))

    for count in BENCH_POINT_COUNTS:
        print("benchmark: atom step, points=%d..." % count)
        _run_atom_step(fb, buf, d, count, radial_tables, pixels_per_bohr, angle, tilt_angle, roll_angle)

    # Same warm-up idea for cloud_common.py's _ORBITAL_SAMPLER_CACHE, for the fixed 2p_z preset.
    print("benchmark: warming orbital sampler cache (2p_z)...")
    n0, ell0, m0, _label0 = cloud_common.ORBITAL_PRESETS[BENCH_ORBITAL_PRESET_INDEX]
    t0 = time.ticks_ms()
    cloud_common.build_point_cloud(n0, ell0, m0, count=BENCH_POINT_COUNTS[0], seed=BENCH_SEED)
    print("BENCH,TABLEBUILD,kind,orbital,warm_ms,%d" % time.ticks_diff(time.ticks_ms(), t0))

    for count in BENCH_POINT_COUNTS:
        print("benchmark: orbital step, points=%d..." % count)
        _run_orbital_step(fb, buf, d, count, angle, tilt_angle, roll_angle)

    gc.collect()
    print("BENCH,MEM,end,heap_free,%d" % gc.mem_free())
    print("BENCH,DONE")


if __name__ == '__main__':
    run()
