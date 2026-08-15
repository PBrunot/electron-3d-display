"""First animation for CLAUDE.md milestone M1/M2: a hydrogen n=2 orbital
point cloud, rendered with per-point brightness proportional to local
probability amplitude, a fixed proton marker at the origin, and a slowly
rotating + dollying (zoom in/out) camera. Built on top of pointcloud.py's
inverse-CDF sampler and orbitals.py's wavefunction math (both already
cross-validated against the C++/JS ports -- see tools/orbitals_host/).

Orbital choice: (n=2, ell=1, m=0), i.e. 2p_z -- a dumbbell along z, picked
over 2s (n=2, ell=0) specifically *because* it is not spherically symmetric,
so the slow rotation is visibly doing something (a rotating 2s cloud would
look static). Trivial to swap: change ORBITAL_N/ELL/M below.

run() opens with a one-time intro fly-over -- camera starts tight on the
proton (INTRO_START_SCALE) and eases back to the steady-state scale over
INTRO_FRAMES frames -- before settling into the indefinite rotate+zoom loop,
so the viewer's eye is anchored on the nucleus before the full cloud comes
into view rather than seeing everything at once from frame 1.

Rendering pipeline (CLAUDE.md section 5): orthographic projection, rotation
about the vertical (Y) axis recomputed from a running angle each frame
(not applied incrementally point-to-point, so there is no drift), no
depth-sort/z-buffer (CLAUDE.md section 5 explicitly notes this is fine for a
sparse point cloud). Points are pre-sampled and their colors precomputed
once in build_point_cloud()/point_colors() -- the per-frame loop only
rotates, projects, and draws, with no heap allocation, matching CLAUDE.md
section 9's "no dynamic allocation in loop()" convention translated to
MicroPython (array.array buffers allocated once up front).

Double-buffered via framebuf.FrameBuffer + ST7789.blit_buffer(), the
MicroPython equivalent of the C++ port's TFT_eSprite double buffer
(CLAUDE.md section 5) -- draws into an off-screen RGB565 buffer, then pushes
the whole frame over SPI in one call, avoiding visible per-point tearing.

Byte-order gotcha (verified on-device, see git history for the probe):
framebuf's RGB565 format stores each pixel little-endian in the buffer, but
st7789py's blit_buffer() sends buffer bytes to the panel as-is over SPI, and
the panel expects big-endian (MSB-first) pixels -- confirmed against
st7789py's own `_ENCODE_PIXEL = ">H"` used by its non-buffered draw calls
(fill_rect/pixel/etc, needs_swap=False on this build, see display.py). So
every color used with the framebuffer must be pre-byte-swapped
(swap16() below) before being handed to fb.pixel()/fb.fill_rect() --
otherwise red and blue-ish channels come out swapped, the same symptom as
the panel's RGB/BGR issue but with a different root cause (framebuf
endianness, not the panel's color order).

180 degree prism-viewing offset: applied per-point via display.to_physical()
(see that module's docstring), same as corner_test.py -- NOT baked into the
rotation math here, so the camera-rotation angle always means the same
physical thing regardless of how the panel is mounted.

Nudge-to-switch-orbital: run() polls nudge.NudgeDetector once per frame
(see that module -- it separates a short sharp push from the slow
reorientation of tilting the board to view a different pyramid face) and,
on a detected L/R/U/D nudge, advances through ORBITAL_PRESETS below and
rebuilds the point cloud for the new orbital. IMU init is wrapped in
try/except: on a board where the QMI8658 isn't wired or doesn't answer,
this prints a warning and the animation still runs, just without nudge
control -- matches the rest of this codebase's stance of not letting an
optional hardware feature take down the core demo.

Fixed-point per-point loop (2026-08-15, prompted by the frame rate at
N_POINTS=3000 being unusably slow): the rotate+project inner loop -- the
part that runs once per point, per frame -- was measured on-device at
~124ms/frame under @micropython.native (float array reads + a fb.pixel()
Python call per point), against ~25ms/frame for the whole-buffer SPI blit.
So the point loop, not the SPI transfer, was the bottleneck.

@micropython.viper was the next step up, but this MicroPython build's
viper does NOT support float->int truncation (`int(some_float)` inside a
viper function raises "can't convert float to int", confirmed live on
device -- a real limitation here, not a theory) and has no viper-level
`float` type annotation at all ("unknown type 'float'"). So instead of
floats, _render_points() below works entirely in Q8 fixed-point integers
(scale factor 256, see FX_SCALE): coordinates are pre-converted to
array.array('i') once per preset load (_to_fixed(), in _load_preset()),
and cos/sin/scale are converted to fixed-point once per *frame* (cheap,
plain Python int(x*256) -- outside viper, where float truncation works
fine) rather than per point. The inner loop then does only integer
multiply/shift, reading xs/ys/zs/colors through ptr32/ptr16 casts (raw
memory access, no per-element boxing) and writing pixels directly into the
framebuffer's backing bytearray via ptr16, bypassing fb.pixel()'s Python
call and internal bounds/format dispatch entirely. Measured result: ~4ms
for the same 3000-point loop that took ~124ms before -- roughly 30x, and
now well under the SPI blit cost, which is the real floor from here (see
CLAUDE.md section 5's ~23ms/frame theoretical minimum at 40MHz SPI).

fb.fill(0)/fb.fill_rect() (clear + proton marker) still go through
framebuf as before -- only the per-point loop needed this treatment, and
those two calls are already cheap (bulk C operations, not once-per-point).
Both write into the same backing `buf` bytearray that _render_points()
writes into directly, so ordering (fill, proton, then points) still
composites correctly on one shared buffer.
"""

import array
import math
import time

import framebuf

import display as display_mod
import orbitals
import pointcloud
import st7789py as st7789

try:
    import nudge
    import qmi8658
except ImportError:
    nudge = None
    qmi8658 = None

WIDTH = display_mod.WIDTH
HEIGHT = display_mod.HEIGHT
CENTER = WIDTH // 2  # panel is square, so this serves both axes

N_POINTS = 3000
ORBITAL_N = 2
ORBITAL_ELL = 1
ORBITAL_M = 0

# Orbitals cycled through on a nudge (see nudge.py / NudgeDetector below).
# (n, ell, m, label) -- label is what TITLE_TEXT shows in place of the raw
# quantum numbers. Kept to n<=3 with closed-ish forms per ORBITALI.md's
# on-device recommendation (higher n works too, orbitals.py's Laguerre
# coefficients aren't hardcoded to n<=3 like the C++ port's; point density
# above isn't tuned past n=3, but projection scale is -- see
# _scale_from_radii() below, which reads each preset's actual sampled
# radii rather than assuming n alone determines them).
# Index 2 is (2, 1, 0), i.e. 2p_z -- the module's original default orbital,
# so a build that never nudges looks exactly as before.
ORBITAL_PRESETS = (
    (1, 0, 0, "1s"),
    (2, 0, 0, "2s"),
    (2, 1, 0, "2p_z"),
    (2, 1, 1, "2p (m=+1)"),
    (3, 1, 0, "3p_z"),
    (3, 2, 0, "3d_z2"),
)
DEFAULT_PRESET_INDEX = 2

# Pixels per orbital-radius-unit for the orthographic projection, derived
# per preset from that preset's *actual* sampled point radii (see
# _scale_from_radii() below) rather than a single global constant or an
# n-only approximation. Two rounds of tuning motivated this:
#
# 1. (2026-08-15) The original fixed BASE_PROJECTION_SCALE=9 -- picked from
#    an on-device 2000-point stats probe of just the n=2 default orbital
#    (median r=4.55, p90=8.15, p99=12.0, max=14.7) -- put the median point
#    only ~41px out of the 120px half-width: a small cluster hugging screen
#    center with a lot of empty black margin, reported as "too far away" on
#    real hardware.
# 2. Scaling that single n=2 probe by an n^2 approximation for other n
#    (the previous _scale_for_n()) only accounts for how pointcloud.py
#    bounds its sampling domain (max_r = 6*n*n) -- it ignores that l and m
#    also shape the *actual* radial distribution (e.g. higher-l orbitals'
#    angular nodes concentrate density differently), so it was still a
#    guess, not a measurement, for every preset except the one it was
#    tuned on.
#
# _scale_from_radii() fixes both: every preset's own sampled points are
# measured directly (P90_TARGET_PX below), so the camera distance is tied
# to that orbital's real characteristics, not to n alone or to a constant
# inherited from a different orbital.
P90_TARGET_PX = 100.0  # where the 90th-percentile-radius point should land
ZOOM_AMPLITUDE_FRACTION = 0.23  # dolly swing as a fraction of base_scale


def _scale_from_radii(xs, ys, zs, target_px=P90_TARGET_PX, percentile=0.90,
                       amplitude_fraction=ZOOM_AMPLITUDE_FRACTION):
    """Projection scale (base, amplitude) measured from this preset's own
    sampled points: finds the `percentile`-th radius among (xs, ys, zs) and
    picks a scale so that radius lands at `target_px` out of screen center.
    amplitude is kept proportional to the resulting base scale, so the zoom
    breathing (see BASE_PROJECTION_SCALE's old comment, now folded in here)
    never shrinks a preset down to a "far away"-looking dot regardless of
    that orbital's absolute size.
    """
    count = len(xs)
    radii = sorted(math.sqrt(xs[i] * xs[i] + ys[i] * ys[i] + zs[i] * zs[i]) for i in range(count))
    idx = min(count - 1, int(percentile * (count - 1)))
    r_ref = radii[idx] if radii[idx] > 1e-6 else 1.0
    base_scale = target_px / r_ref
    return base_scale, base_scale * amplitude_fraction


# Slow rotation about the vertical (Y) axis. Measured on-device: render+blit
# alone runs at ~46ms/frame (~21.7 fps, already within CLAUDE.md's 20-30fps
# M1 target -- see PR discussion), so FRAME_DELAY_MS is a small cooperative
# yield, not a real throttle. At that pace, 0.015 rad/frame is roughly one
# full revolution every ~19s.
ANGLE_STEP = 0.030
FRAME_DELAY_MS = 5

# Zoom breathing is slower than the rotation and on an independent phase, so
# the two don't fall into a repeating combined cycle: ~0.008 rad/frame is
# roughly one full in-out cycle every ~40s at ~21.7 fps.
ZOOM_ANGLE_STEP = 0.016

# One-time intro fly-over (see run()): starts at INTRO_START_SCALE (tight on
# the proton) and eases to the loaded preset's base_scale over INTRO_FRAMES
# frames (base_scale now comes from _scale_from_radii(), see above). At
# ~1500 points render+blit measures well under 100ms/frame on-device, so
# 200 frames reads as a deliberate pull-back, not a wait before "the real
# thing" starts.
INTRO_START_SCALE = 100.0
INTRO_FRAMES = 200

# Same fly-over effect (see _fly_over() below), reused on every nudge-
# triggered orbital switch so the camera visibly zooms in on the new cloud
# instead of cutting straight to the steady-state view. Relative to the new
# preset's own base_scale (SWITCH_START_SCALE_FACTOR times it) rather than a
# fixed value like INTRO_START_SCALE, so the "start tight" effect scales
# with whichever orbital was just switched to. Shorter than the one-time
# intro (SWITCH_TRANSITION_FRAMES << INTRO_FRAMES) -- this happens every
# switch, not once at boot, so it needs to read as a snappy zoom-in, not a
# repeat of the slow reveal.
SWITCH_START_SCALE_FACTOR = 6.0
SWITCH_TRANSITION_FRAMES = 40

# Sampling RNG seed: fixed for a reproducible-looking demo across boots
# (this is a visual demo, not one of the cross-validated test cases in
# tools/orbitals_host/, so there's no requirement it match another port).
SEED = 12345

PROTON_SIZE = 3  # side length in px of the filled square marking the proton

# FPS overlay: refreshed every FPS_UPDATE_INTERVAL frames (not every frame)
# so the counter is a rolling average, not the noisy per-frame timing, and
# so the text() calls needed to compute/draw it don't themselves become a
# per-frame cost. Drawn in raw sprite-space top-left, NOT through
# display.to_physical() like everything else here -- to_physical is a pure
# per-pixel coordinate remap, and a coordinate remap alone cannot make text
# physically readable through the 180 degree prism offset (the glyphs
# themselves would need to be rotated, which framebuf.text() can't do); this
# is a dev/debug overlay, so it's left in panel-native orientation rather
# than spending effort on glyph rotation for it.
FPS_UPDATE_INTERVAL = 50
FPS_TEXT_POS = (2, 2)

# Title overlay: which orbital preset is currently showing. Rebuilt (not a
# module-level constant anymore) whenever run() switches presets on a nudge
# -- see _title_for_preset() below. Same sprite-space/non-flipped caveat as
# FPS_TEXT_POS.
TITLE_TEXT_POS = (2, 12)
LOADING_TEXT = "Loading..."
LOADING_TEXT_POS = (2, 22)


def _title_for_preset(preset):
    n, ell, m, label = preset
    return "%s (n=%d l=%d m=%d)" % (label, n, ell, m)


def swap16(color565):
    """Byte-swap a 16-bit RGB565 value -- see module docstring's byte-order
    gotcha. Every color drawn into the framebuffer must be pre-swapped.
    """
    return ((color565 & 0xFF) << 8) | (color565 >> 8)


def build_point_cloud(n=ORBITAL_N, ell=ORBITAL_ELL, m=ORBITAL_M, count=N_POINTS, seed=SEED):
    """Sample `count` points from the (n, ell, m) orbital's probability
    density, plus the (unsigned, unnormalized) psi^2 amplitude at each
    sampled point -- the latter used by point_colors() for brightness.

    Returns:
        (xs, ys, zs, psi2): four array.array('f') of length `count`.
    """
    sampler = pointcloud.init_orbital_sampler(n, ell, m)
    rng = pointcloud.XorShift32(seed)

    radial_coeff = orbitals.laguerre_coeffs(n, ell)
    legendre_coeff = orbitals.legendre_coeffs(ell, m)

    xs = array.array('f', bytes(4 * count))
    ys = array.array('f', bytes(4 * count))
    zs = array.array('f', bytes(4 * count))
    psi2 = array.array('f', bytes(4 * count))

    for i in range(count):
        x, y, z = pointcloud.sample_orbital_point(sampler, rng)
        xs[i] = x
        ys[i] = y
        zs[i] = z

        r = math.sqrt(x * x + y * y + z * z)
        theta = math.acos(z / r) if r > 1e-9 else 0.0
        phi = math.atan2(y, x)
        psi = orbitals.psi_real(r, theta, phi, n, ell, m, radial_coeff, legendre_coeff)
        psi2[i] = psi * psi

    return xs, ys, zs, psi2


def point_colors(psi2, min_level=60, max_level=255):
    """Map each point's psi^2 amplitude to a pre-byte-swapped grayscale-ish
    RGB565 color (brightness only, per CLAUDE.md section 8's default
    pending a decision on phase-to-color mapping). min_level keeps the
    dimmest points visible instead of fading to invisible-on-black.

    Rank-based (histogram-equalized), NOT linear min/max: psi^2 for a
    hydrogen-like orbital is heavily right-skewed among inverse-CDF sampled
    points -- a handful of samples land very close to the nucleus where the
    density spikes, while the bulk of the (still valid, still sampled)
    points sit at much lower density further out. A linear (psi2-lo)/(hi-lo)
    scale is dominated by that handful of outliers: it technically touches
    both min_level and max_level, but most points end up compressed into
    the bottom slice of the range and only a few near the top -- the full
    color spectrum exists but isn't actually *used* by most points. Sorting
    by psi2 and assigning brightness by rank/count instead guarantees an
    even spread across the whole [min_level, max_level] range regardless of
    how skewed the underlying values are (every rank bucket gets used
    equally by construction).

    O(count log count) once per preset load (see _load_preset()), not
    per-frame -- fine at N_POINTS=1500, no need for anything cleverer.
    """
    count = len(psi2)
    ranked = [(psi2[i], i) for i in range(count)]
    ranked.sort()

    span = max_level - min_level
    denom = count - 1 if count > 1 else 1

    colors = array.array('H', bytes(2 * count))
    for rank in range(count):
        i = ranked[rank][1]
        level = min_level + (rank * span) // denom
        colors[i] = swap16(st7789.color565(level // 3, level // 2, level))
    return colors


FX_BITS = 8
FX_SCALE = 1 << FX_BITS  # 256; see module docstring's "Fixed-point per-point loop"


def _to_fixed(values):
    """array.array('f') -> Q8 fixed-point array.array('i') (multiply by
    FX_SCALE, truncate). Called once per preset load (_load_preset()), not
    per frame -- see module docstring.
    """
    count = len(values)
    out = array.array('i', bytes(4 * count))
    for i in range(count):
        out[i] = int(values[i] * FX_SCALE)
    return out


@micropython.viper
def _render_points(buf, xs, ys, zs, colors, n: int, cos_fx: int, sin_fx: int, scale_fx: int,
                    cx: int, cy: int, w: int, h: int):
    """Rotate, project, and draw every point directly into `buf` (the
    framebuffer's backing bytearray) -- Q8 fixed-point integer math only,
    see the module docstring's "Fixed-point per-point loop" for why (this
    build's viper can't truncate a float to int, so genuine per-point float
    math isn't an option here). xs/ys/zs/colors/buf are cast to raw pointers
    (ptr32 for the Q8 coordinate arrays, ptr16 for colors and the pixel
    buffer) so every array access is a direct memory read/write, no
    per-element object boxing and no fb.pixel() call.

    Bit accounting: x/y/z/cos_fx/sin_fx/scale_fx are all Q8 (value * 256).
    Multiplying two Q8 values gives Q16, so `>> 8` after the rotation sum
    brings the Q16 intermediate back to Q8 (matching the coordinate
    arrays' scale), and `>> 16` after the projection multiply collapses
    Q16 all the way to a plain pixel integer. The shift amounts are
    literal ints, NOT the module-level FX_BITS constant -- referencing a
    module global from inside a viper function returns a boxed 'object',
    and mixing that with the native 'int' arithmetic below fails to
    compile ("can't do binary op between 'int' and 'object'", confirmed
    live on device). Must be kept in sync with FX_BITS = 8 by hand if that
    constant ever changes.
    """
    pxs = ptr32(xs)
    pys = ptr32(ys)
    pzs = ptr32(zs)
    pcolors = ptr16(colors)
    pbuf = ptr16(buf)
    i = 0
    while i < n:
        x = pxs[i]
        y = pys[i]
        z = pzs[i]

        # Rotate about the vertical (Y) axis; y is unaffected.
        rx = (x * cos_fx + z * sin_fx) >> 8

        # Orthographic projection; screen y grows downward, so flip.
        sx = cx + ((rx * scale_fx) >> 16)
        sy = cy - ((y * scale_fx) >> 16)

        if sx >= 0 and sx < w and sy >= 0 and sy < h:
            pbuf[(h - 1 - sy) * w + (w - 1 - sx)] = pcolors[i]
        i += 1


def _render_frame(fb, buf, xs_fx, ys_fx, zs_fx, colors, proton_color, angle, scale):
    """Clear `fb`/`buf` and draw one frame: the proton marker (still via
    framebuf -- cheap, not once-per-point) plus every point in
    (xs_fx, ys_fx, zs_fx) (Q8 fixed-point, see _to_fixed()), rotated by
    `angle` and projected at `scale` px/unit. Shared by both the fly-over
    (_fly_over()) and the steady-state loop in run() so the two can't drift
    into inconsistent rendering.

    cos/sin/scale are converted to Q8 fixed-point here, once per frame
    (plain Python int(float * FX_SCALE) -- this works fine outside viper;
    see module docstring for why it does NOT work inside _render_points()
    itself), then handed to the viper point loop.
    """
    width = WIDTH
    height = HEIGHT
    center = CENTER
    w1 = width - 1
    h1 = height - 1

    fb.fill(0)

    # Proton: fixed at the origin, which projects to screen center at every
    # camera angle (rotation is about the origin) and is unaffected by
    # zoom scale, so it never moves -- only the electron cloud spins/zooms.
    proton_sprite_x = center - PROTON_SIZE // 2
    proton_sprite_y = center - PROTON_SIZE // 2
    fb.fill_rect(w1 - proton_sprite_x, h1 - proton_sprite_y, PROTON_SIZE, PROTON_SIZE, proton_color)

    cos_fx = int(math.cos(angle) * FX_SCALE)
    sin_fx = int(math.sin(angle) * FX_SCALE)
    scale_fx = int(scale * FX_SCALE)
    _render_points(buf, xs_fx, ys_fx, zs_fx, colors, len(xs_fx), cos_fx, sin_fx, scale_fx,
                    center, center, width, height)


def _fly_over(d, fb, buf, xs_fx, ys_fx, zs_fx, colors, proton_color, text_color, title_text,
              angle, start_scale, end_scale, frames):
    """Ease the projection scale from start_scale to end_scale over `frames`
    frames, rendering+blitting each one -- the camera visibly zooming in (or
    out) rather than cutting straight to end_scale. Shared by run()'s
    one-time boot intro and every nudge-triggered orbital switch (see
    SWITCH_START_SCALE_FACTOR/SWITCH_TRANSITION_FRAMES) so the two can't
    drift into inconsistent easing, same rationale as _render_frame()
    itself being shared. Returns the running `angle` after the last frame,
    so the caller's rotation continues smoothly into the steady-state loop.

    No FPS overlay during the fly-over -- it's a short, one-time-per-call
    transition, not the steady-state rate FPS_TEXT is meant to report.
    """
    two_pi = 2 * math.pi
    for i in range(frames):
        t = i / (frames - 1) if frames > 1 else 1.0
        scale = start_scale + (end_scale - start_scale) * t
        _render_frame(fb, buf, xs_fx, ys_fx, zs_fx, colors, proton_color, angle, scale)
        fb.text(title_text, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
        angle += ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        time.sleep_ms(FRAME_DELAY_MS)
    return angle


def _load_preset(index):
    """Build the point cloud, colors, title text, and projection
    scale/amplitude for ORBITAL_PRESETS[index]. Shared by run()'s initial
    load and every nudge-triggered switch so the two can't drift apart.

    Returns Q8 fixed-point coordinate arrays (xs_fx, ys_fx, zs_fx), not
    float -- see the module docstring's "Fixed-point per-point loop".
    _scale_from_radii() still runs on the original float xs/ys/zs (radius
    math there is a one-time per-load cost, not the per-frame hot loop that
    motivated fixed point, so there's no reason to fixed-point it too).

    Timed and printed to serial (sampling vs. color-mapping split out)
    since this is the one place in the loop slow enough (~1.5-2.5s measured
    on-device, all interpreted Python -- see the LOADING_TEXT comment in
    run()) that knowing where the time actually goes matters for anyone
    tuning N_POINTS or adding a preset.
    """
    n, ell, m, label = ORBITAL_PRESETS[index]
    print("orbital: loading preset %d (%s, n=%d l=%d m=%d)..." % (index, label, n, ell, m))
    t0 = time.ticks_ms()
    xs, ys, zs, psi2 = build_point_cloud(n, ell, m)
    t1 = time.ticks_ms()
    colors = point_colors(psi2)
    t2 = time.ticks_ms()
    title = _title_for_preset(ORBITAL_PRESETS[index])
    base_scale, zoom_amplitude = _scale_from_radii(xs, ys, zs)
    xs_fx = _to_fixed(xs)
    ys_fx = _to_fixed(ys)
    zs_fx = _to_fixed(zs)
    t3 = time.ticks_ms()
    print("orbital: %s loaded in %dms (sample=%dms color=%dms fixed=%dms) scale=%.1f" % (
        label, time.ticks_diff(t3, t0), time.ticks_diff(t1, t0), time.ticks_diff(t2, t1),
        time.ticks_diff(t3, t2), base_scale))
    return xs_fx, ys_fx, zs_fx, colors, title, base_scale, zoom_amplitude


def _init_nudge_detector():
    """Best-effort IMU + nudge detector setup. Returns None (with a printed
    warning) if the QMI8658 isn't present/answering -- an optional feature
    failing shouldn't take the animation down with it.
    """
    if qmi8658 is None or nudge is None:
        print("nudge: qmi8658/nudge modules not found, orbital switching via nudge disabled")
        return None
    try:
        imu = qmi8658.QMI8658()
        detector = nudge.NudgeDetector(imu)
        print("nudge: QMI8658 ready, nudge-controlled orbital switching enabled")
        return detector
    except OSError as e:
        print("nudge: IMU init failed (%s), orbital switching via nudge disabled" % e)
        return None


# Direction -> preset index step. Both directions per axis pair advance the
# same way (R/U forward, L/D backward) rather than assigning independent
# meaning to each of the 4 directions -- simplest mapping that satisfies
# "nudge in any of the 4 directions changes the animation", and matches
# nudge.py's current pre-calibration axis mapping being a placeholder (see
# that module's docstring) -- no reason yet to burn a more elaborate mapping
# on an unverified axis assignment.
_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


def run():
    print("orbital: display init...")
    d = display_mod.init()
    print("orbital: display ready, %d presets available" % len(ORBITAL_PRESETS))

    preset_index = DEFAULT_PRESET_INDEX
    xs, ys, zs, colors, title_text, base_scale, zoom_amplitude = _load_preset(preset_index)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = swap16(st7789.color565(255, 0, 0))
    text_color = swap16(st7789.color565(255, 255, 255))

    detector = _init_nudge_detector()

    angle = 0.0
    zoom_angle = 0.0
    two_pi = 2 * math.pi

    # Intro fly-over: start zoomed in tight on the proton (INTRO_START_SCALE
    # is far past base_scale, so only the proton marker and the handful of
    # points nearest the origin are inside the frame -- most of the cloud is
    # scaled off-screen and simply skipped by the bounds check in
    # _render_frame), then pull back over INTRO_FRAMES frames to the
    # steady-state scale. Anchors the viewer on the nucleus before the full
    # cloud is revealed, rather than showing everything at once from frame 1.
    angle = _fly_over(d, fb, buf, xs, ys, zs, colors, proton_color, text_color, title_text,
                       angle, INTRO_START_SCALE, base_scale, INTRO_FRAMES)

    # FPS counter: a rolling average over FPS_UPDATE_INTERVAL frames (see
    # that constant's comment for why it's not recomputed every frame).
    fps_text = "FPS: --"
    frame_count = 0
    fps_window_start = time.ticks_ms()

    while True:
        # Nudge check: once per frame, cheap (a handful of I2C bytes, see
        # qmi8658.py) relative to the render+blit cost measured above. A
        # detected nudge switches ORBITAL_PRESETS, rebuilds the point cloud
        # -- inherently a ~1.5s stall at N_POINTS=1500 (measured on-device;
        # build_point_cloud() runs interpreted trig per point, not
        # @micropython.native like _render_frame), so a LOADING_TEXT frame
        # is pushed first rather than leaving the display frozen on the old
        # cloud with no feedback -- and then zooms into the new cloud via
        # _fly_over() (same effect as the boot intro, see
        # SWITCH_START_SCALE_FACTOR/SWITCH_TRANSITION_FRAMES) instead of
        # cutting straight to the steady-state view.
        if detector is not None:
            raw = detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                step = _NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    preset_index = (preset_index + step) % len(ORBITAL_PRESETS)
                    fb.fill(0)
                    fb.text(LOADING_TEXT, LOADING_TEXT_POS[0], LOADING_TEXT_POS[1], text_color)
                    d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
                    xs, ys, zs, colors, title_text, base_scale, zoom_amplitude = _load_preset(preset_index)
                    angle = _fly_over(d, fb, buf, xs, ys, zs, colors, proton_color, text_color, title_text,
                                       angle, base_scale * SWITCH_START_SCALE_FACTOR, base_scale,
                                       SWITCH_TRANSITION_FRAMES)

        scale = base_scale + zoom_amplitude * math.sin(zoom_angle)
        _render_frame(fb, buf, xs, ys, zs, colors, proton_color, angle, scale)
        fb.text(title_text, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        fb.text(fps_text, FPS_TEXT_POS[0], FPS_TEXT_POS[1], text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        frame_count += 1
        if frame_count >= FPS_UPDATE_INTERVAL:
            now = time.ticks_ms()
            elapsed_ms = time.ticks_diff(now, fps_window_start)
            fps = 1000.0 * frame_count / elapsed_ms if elapsed_ms > 0 else 0.0
            fps_text = "FPS: %.1f" % fps
            print("orbital: %s" % fps_text)
            frame_count = 0
            fps_window_start = now

        angle += ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        zoom_angle += ZOOM_ANGLE_STEP
        if zoom_angle >= two_pi:
            zoom_angle -= two_pi
        time.sleep_ms(FRAME_DELAY_MS)


if __name__ == '__main__':
    run()
