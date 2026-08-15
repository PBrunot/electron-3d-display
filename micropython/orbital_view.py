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
"""

import array
import math
import time

import framebuf

import display as display_mod
import orbitals
import pointcloud
import st7789py as st7789

WIDTH = display_mod.WIDTH
HEIGHT = display_mod.HEIGHT
CENTER = WIDTH // 2  # panel is square, so this serves both axes

N_POINTS = 600
ORBITAL_N = 2
ORBITAL_ELL = 1
ORBITAL_M = 0

# Pixels per orbital-radius-unit for the orthographic projection. Fixed
# (not auto-scaled from the sampled points' max radius) so a single unlucky
# long-tail sample can't compress the whole cloud -- derived from an
# on-device 2000-point statistics probe of this exact orbital: median
# r=4.55, p90=8.15, p99=12.0, max=14.7 (see PR discussion).
#
# The camera also "dollies" in and out: effective scale oscillates as
# BASE_PROJECTION_SCALE + ZOOM_AMPLITUDE*sin(zoom_angle), independently of
# the rotation angle (different speed, see ZOOM_ANGLE_STEP below). Range
# [4, 14]: at the near end (14) p99 lands ~168px out (past the 120px
# half-width, so the rare long tail clips off-screen briefly -- fine, points
# outside bounds are just skipped, no crash); at the far end (4) the median
# point is only ~18px out, visibly "farther away", giving the breathing
# effect its contrast.
BASE_PROJECTION_SCALE = 9.0
ZOOM_AMPLITUDE = 5.0

# Slow rotation about the vertical (Y) axis. Measured on-device: render+blit
# alone runs at ~46ms/frame (~21.7 fps, already within CLAUDE.md's 20-30fps
# M1 target -- see PR discussion), so FRAME_DELAY_MS is a small cooperative
# yield, not a real throttle. At that pace, 0.015 rad/frame is roughly one
# full revolution every ~19s.
ANGLE_STEP = 0.015
FRAME_DELAY_MS = 5

# Zoom breathing is slower than the rotation and on an independent phase, so
# the two don't fall into a repeating combined cycle: ~0.008 rad/frame is
# roughly one full in-out cycle every ~40s at ~21.7 fps.
ZOOM_ANGLE_STEP = 0.008

# One-time intro fly-over (see run()): starts at INTRO_START_SCALE (tight on
# the proton) and eases to BASE_PROJECTION_SCALE over INTRO_FRAMES frames.
# At ~600 points render+blit measures ~98ms/frame on-device, so 50 frames is
# roughly a 5s intro -- long enough to read as a deliberate pull-back, short
# enough not to feel like a wait before "the real thing" starts.
INTRO_START_SCALE = 40.0
INTRO_FRAMES = 50

# Sampling RNG seed: fixed for a reproducible-looking demo across boots
# (this is a visual demo, not one of the cross-validated test cases in
# tools/orbitals_host/, so there's no requirement it match another port).
SEED = 12345

PROTON_SIZE = 4  # side length in px of the filled square marking the proton

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
FPS_UPDATE_INTERVAL = 15
FPS_TEXT_POS = (2, 2)

# Title overlay: quantum numbers of whatever orbital ORBITAL_N/ELL/M above
# is currently set to, so the label can't drift out of sync if those are
# changed -- built once at import time (not reconstructed every frame) since
# it's static for the life of the process. Same sprite-space/non-flipped
# caveat as FPS_TEXT_POS.
TITLE_TEXT = "n=%d l=%d m=%d" % (ORBITAL_N, ORBITAL_ELL, ORBITAL_M)
TITLE_TEXT_POS = (2, 12)


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
    """Map each point's psi^2 amplitude to a pre-byte-swapped grayscale
    RGB565 color (monochrome intensity, per CLAUDE.md section 8's default
    pending a decision on phase-to-color mapping). min_level keeps the
    dimmest points visible instead of fading to invisible-on-black.
    """
    lo = min(psi2)
    hi = max(psi2)
    span = hi - lo if hi > lo else 1.0

    count = len(psi2)
    colors = array.array('H', bytes(2 * count))
    for i in range(count):
        frac = (psi2[i] - lo) / span
        level = int(min_level + frac * (max_level - min_level))
        colors[i] = swap16(st7789.color565(level, level, level))
    return colors


@micropython.native
def _render_frame(fb, xs, ys, zs, colors, proton_color, angle, scale):
    """Clear `fb` and draw one frame: the proton marker plus every point in
    (xs, ys, zs), rotated by `angle` about the vertical (Y) axis and
    orthographically projected at `scale` px/unit. Shared by both the intro
    fly-over and the steady-state loop in run() so the two can't drift into
    inconsistent rendering.

    @micropython.native (see orbitals.py's docstring for the rationale) plus
    two changes made after the first version of this loop measured ~98ms at
    600 points (unusably choppy, per user report on real hardware):
    display_mod.to_physical(sx, sy) is inlined as `w1 - sx, h1 - sy` instead
    of called -- a function call plus tuple allocate-and-unpack per point,
    600 times a frame, was real, measurable overhead; the transform is
    display.py's WIDTH-1-x/HEIGHT-1-y, duplicated here only for that reason,
    kept in sync by inspection since it's a two-line identity that hasn't
    changed since it was established (see display.py's own docstring). And
    the previous version's `rz = -x*s + z*c` was computed but never used
    (orthographic projection only needs rx and y) -- dead arithmetic, removed.
    """
    width = WIDTH
    height = HEIGHT
    center = CENTER
    w1 = width - 1
    h1 = height - 1

    c = math.cos(angle)
    s = math.sin(angle)

    fb.fill(0)

    # Proton: fixed at the origin, which projects to screen center at every
    # camera angle (rotation is about the origin) and is unaffected by
    # zoom scale, so it never moves -- only the electron cloud spins/zooms.
    proton_sprite_x = center - PROTON_SIZE // 2
    proton_sprite_y = center - PROTON_SIZE // 2
    fb.fill_rect(w1 - proton_sprite_x, h1 - proton_sprite_y, PROTON_SIZE, PROTON_SIZE, proton_color)

    pixel = fb.pixel
    n = len(xs)
    for i in range(n):
        x = xs[i]
        y = ys[i]
        z = zs[i]

        # Rotate about the vertical (Y) axis; y is unaffected.
        rx = x * c + z * s

        # Orthographic projection; screen y grows downward, so flip.
        sx = center + int(rx * scale)
        sy = center - int(y * scale)

        if 0 <= sx < width and 0 <= sy < height:
            pixel(w1 - sx, h1 - sy, colors[i])


def run():
    d = display_mod.init()

    xs, ys, zs, psi2 = build_point_cloud()
    colors = point_colors(psi2)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = swap16(st7789.color565(255, 0, 0))
    text_color = swap16(st7789.color565(255, 255, 255))

    angle = 0.0
    zoom_angle = 0.0
    two_pi = 2 * math.pi

    # Intro fly-over: start zoomed in tight on the proton (INTRO_START_SCALE
    # is far past BASE_PROJECTION_SCALE, so only the proton marker and the
    # handful of points nearest the origin are inside the frame -- most of
    # the cloud is scaled off-screen and simply skipped by the bounds check
    # in _render_frame), then pull back over INTRO_FRAMES frames to the
    # steady-state scale. Anchors the viewer on the nucleus before the full
    # cloud is revealed, rather than showing everything at once from frame 1.
    # (No FPS overlay during the intro -- it's a short, one-time transition,
    # not the steady-state rate the counter is meant to report.)
    for i in range(INTRO_FRAMES):
        t = i / (INTRO_FRAMES - 1)
        scale = INTRO_START_SCALE + (BASE_PROJECTION_SCALE - INTRO_START_SCALE) * t
        _render_frame(fb, xs, ys, zs, colors, proton_color, angle, scale)
        fb.text(TITLE_TEXT, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
        angle += ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        time.sleep_ms(FRAME_DELAY_MS)

    # FPS counter: a rolling average over FPS_UPDATE_INTERVAL frames (see
    # that constant's comment for why it's not recomputed every frame).
    fps_text = "FPS: --"
    frame_count = 0
    fps_window_start = time.ticks_ms()

    while True:
        scale = BASE_PROJECTION_SCALE + ZOOM_AMPLITUDE * math.sin(zoom_angle)
        _render_frame(fb, xs, ys, zs, colors, proton_color, angle, scale)
        fb.text(TITLE_TEXT, TITLE_TEXT_POS[0], TITLE_TEXT_POS[1], text_color)
        fb.text(fps_text, FPS_TEXT_POS[0], FPS_TEXT_POS[1], text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        frame_count += 1
        if frame_count >= FPS_UPDATE_INTERVAL:
            now = time.ticks_ms()
            elapsed_ms = time.ticks_diff(now, fps_window_start)
            fps = 1000.0 * frame_count / elapsed_ms if elapsed_ms > 0 else 0.0
            fps_text = "FPS: %.1f" % fps
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
