"""PC debug port of micropython/orbital_view.py: same hydrogen-orbital
point-cloud animation, same nudge-to-switch-orbital control, same
point-turnover/"buzz" liveliness effects, running in a tkinter window
instead of on the ESP32-S3 panel -- see pc/README.md for why this exists
and how to run it.

What's genuinely SHARED with the device, not reimplemented (see
micropython_shim.py for how): micropython/orbitals.py (the wavefunction
math) and micropython/pointcloud.py (the inverse-CDF sampler), imported
and run completely unmodified -- these are the parts that are actually
complex and cross-validated (see tools/orbitals_host/), so this is where
avoiding a second copy matters most. micropython/nudge.py's NudgeDetector
is also imported unmodified; only the "sensor" underneath it differs (see
keyboard_imu.py).

What's DUPLICATED from micropython/orbital_view.py, not shared: the thin
orchestration layer around that math -- build_point_cloud(), point_colors()
(same rank-based histogram-equalization algorithm), _scale_from_radii(),
_ResampleState/_resample_points()/_bisect_rank(), ORBITAL_PRESETS, and the
CULL_*/BUZZ_* constants. These can't be imported directly: orbital_view.py
unconditionally imports framebuf/display/st7789py (real ESP32 hardware
drivers, no CPython equivalent) at module scope, so `import orbital_view`
itself fails under CPython regardless of the micropython.native/viper
question micropython_shim.py solves. Splitting those pieces out into a
shared, hardware-agnostic module would remove this duplication -- not done
here to avoid touching the already-verified, deployed device code in the
same change that adds this tool. If you change the algorithm in one place
(e.g. point_colors()'s rank formula, or a preset), change it in both, or
do that split.

What's genuinely DIFFERENT (no ESP32 equivalent to share):
- Rendering target: a bytearray RGB buffer -> PIL Image -> tkinter Canvas,
  not framebuf.FrameBuffer -> ST7789 SPI. No Q8 fixed-point/viper here --
  see orbital_view.py's docstring for why *it* needs that (this build's
  viper can't truncate float to int); CPython has neither that limitation
  nor any reason to avoid plain floats, a desktop CPU is fast enough that
  the ESP32's per-point bottleneck simply isn't one here.
- No 180-degree prism-viewing offset (display.to_physical() on the device):
  this window shows the logical point cloud directly, not what it looks
  like reflected through a physical prism, since there is no prism.
- "Buzz" uses real per-point randomness (random.random() < BUZZ_FRACTION)
  instead of orbital_view.py's integer multiplicative hash -- that hash
  was specifically a workaround for viper having no RNG and no float
  support (see _render_points()'s docstring there), neither constraint
  applies on CPython.
- Nudge input is the keyboard (arrow keys), via keyboard_imu.KeyboardIMU,
  not a real QMI8658.
- N_POINTS is 10000 here vs. the device's 3000 -- no SPI/render-loop
  bottleneck to respect on a desktop CPU (see above), so there's no reason
  to match the device's budget for a tool whose whole purpose is visual
  debugging, where more points read as a richer/less sparse cloud.
- Bounding sphere + rotation-indicator marker (see BOUNDING_SPHERE_COLOR):
  a debug aid with no device equivalent, added because several
  ORBITAL_PRESETS look close to rotationally symmetric about the view axis
  in plain orthographic projection, making the rotation itself hard to
  perceive from the point cloud alone.
"""

import array
import math
import os
import random
import sys
import time

import micropython_shim  # noqa: F401 -- import for its side effect (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import nudge
import orbitals
import pointcloud

from keyboard_imu import KeyboardIMU

from PIL import Image, ImageDraw, ImageFont, ImageTk
import tkinter as tk

WIDTH = 240
HEIGHT = 240
CENTER = WIDTH // 2

DISPLAY_SCALE = 3  # tkinter window is WIDTH*DISPLAY_SCALE square; math stays at WIDTH/HEIGHT
DISPLAY_SIZE = (WIDTH * DISPLAY_SCALE, HEIGHT * DISPLAY_SCALE)

N_POINTS = 10000  # higher than the device's 3000 -- a desktop CPU has the headroom for it
SEED = 12345

# Kept identical to micropython/orbital_view.py -- see that file if you're
# comparing behavior, and this module's docstring for why it's duplicated
# rather than imported.
ORBITAL_PRESETS = (
    (1, 0, 0, "1s"),
    (2, 0, 0, "2s"),
    (2, 1, 0, "2p_z"),
    (2, 1, 1, "2p (m=+1)"),
    (3, 1, 0, "3p_z"),
    (3, 2, 0, "3d_z2"),
)
DEFAULT_PRESET_INDEX = 2

COLOR_MIN_LEVEL = 60
COLOR_MAX_LEVEL = 255

P90_TARGET_PX = 100.0
ZOOM_AMPLITUDE_FRACTION = 0.23

# Bounding sphere + rotation-indicator marker -- PC-only, no device
# equivalent (see module docstring). A point cloud rotating in orthographic
# projection alone is hard to read: for several of ORBITAL_PRESETS the
# silhouette is close to rotationally symmetric about the view (Y) axis, so
# there's little visible change frame to frame even though the cloud IS
# turning. Two fixed reference elements fix that:
#  - A static circle at radius r_ref (the same p90 radius base_scale is
#    calibrated against, see _scale_from_radii()) -- breathes with the
#    zoom like the cloud does, but does NOT rotate (a sphere's silhouette
#    is a circle from every angle), so it's a pure size/extent anchor.
#  - A single reference vector, elevated near the pole rather than lying
#    in the horizontal (X/Z) plane -- see MARKER_ELEVATION_DEG -- rotated
#    by the identical angle each frame and drawn as a single "H" glyph
#    riding on the sphere at its projected tip. A first version put this
#    vector in the X/Z plane (r_ref, 0, 0), sweeping along the equator;
#    moved up near the pole on request ("on top of the sphere", not at
#    the equator) -- see MARKER_ELEVATION_DEG's comment for why a vector
#    exactly ON the Y axis wouldn't work for this. The circle stays put
#    (a sphere's silhouette is a circle from every angle) as a pure
#    size/extent anchor; the marker is what visibly moves, giving an
#    unambiguous, continuous read on both rotation direction and current
#    angle.
BOUNDING_SPHERE_COLOR = (70, 70, 90)
MARKER_TEXT = "H"
MARKER_FONT_SIZE = 15  # PIL's default bitmap font is ~11px and reads as barely-there at DISPLAY_SCALE
# Elevation of the marker's reference vector above the horizontal (X/Z)
# plane, in degrees -- 90 would put it exactly on the Y (rotation) axis,
# which never moves under a Y-axis rotation (rx = x*c + z*s leaves a
# (0, y, 0) point unchanged) and so wouldn't show rotation at all; 0 would
# put it back on the equator. 70 read as too close to the top edge in
# practice -- it collided with the title text overlay (drawn at (2, 2) in
# _blit(), also near the top of the frame). 50 keeps the marker clearly in the
# upper half/reading as "near the top of the sphere" while sitting below
# the title text, with a bigger horizontal swing (cos(50 deg) =~ 0.64 of
# r_ref) than 70 deg would give.
MARKER_ELEVATION_DEG = 50.0
_MARKER_ELEVATION_RAD = math.radians(MARKER_ELEVATION_DEG)
MARKER_COLOR_DIM = 140    # brightness (all of R/G) when the marker is rotating away from the viewer
MARKER_COLOR_BRIGHT = 255  # brightness when rotating toward the viewer
MARKER_BLUE = 40
# Loaded once here, not per-frame in _draw_bounding_sphere_and_marker() --
# ImageFont.load_default(size=...) parses a font every call, needless
# per-frame cost for something that never changes.
_MARKER_FONT = ImageFont.load_default(size=MARKER_FONT_SIZE)

CULL_FRACTION = 0.01
CULL_REFRESH_FRAMES = 3
BUZZ_FRACTION = 0.04

ANGLE_STEP = 0.030
ZOOM_ANGLE_STEP = 0.016
FRAME_DELAY_MS = 20  # tkinter .after() delay; PC is fast enough this is the real throttle

INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 18

PROTON_SIZE = 3
PROTON_COLOR = (255, 0, 0)

_NUDGE_DIRECTION_STEP = {'R': 1, 'U': 1, 'L': -1, 'D': -1}


def build_point_cloud(n, ell, m, count=N_POINTS, seed=SEED):
    """Identical algorithm to micropython/orbital_view.py's function of the
    same name (see this module's docstring) -- sample `count` points from
    the (n, ell, m) orbital's probability density, plus psi^2 at each.
    Also returns sampler/rng/radial_coeff/legendre_coeff so
    _resample_points() can keep drawing from the same distribution later.
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

    return xs, ys, zs, psi2, sampler, rng, radial_coeff, legendre_coeff


def _level_to_rgb(level):
    """Same tint formula as orbital_view.py's _level_to_color() (level//3
    red, level//2 green, level blue -- a slight blue shift), but returns a
    plain (r, g, b) tuple for PIL instead of a pre-byte-swapped RGB565 int
    (that packing is purely an ESP32 panel detail, see orbital_view.py's
    module docstring's byte-order section -- nothing on the PC side needs
    it).
    """
    return (level // 3, level // 2, level)


def point_colors(psi2):
    """Same rank-based histogram-equalization algorithm as
    orbital_view.py's point_colors() -- see that function's docstring for
    the full rationale (psi^2 is heavily right-skewed, so linear min/max
    would compress most points into the dim end of the range). Returns
    (colors, psi2_sorted): colors is a plain list of (r, g, b) tuples;
    psi2_sorted is the frozen sorted reference used by _resample_points().
    """
    count = len(psi2)
    ranked = [(psi2[i], i) for i in range(count)]
    ranked.sort()

    span = COLOR_MAX_LEVEL - COLOR_MIN_LEVEL
    denom = count - 1 if count > 1 else 1

    colors = [None] * count
    psi2_sorted = array.array('f', bytes(4 * count))
    for rank in range(count):
        value, i = ranked[rank]
        psi2_sorted[rank] = value
        level = COLOR_MIN_LEVEL + (rank * span) // denom
        colors[i] = _level_to_rgb(level)
    return colors, psi2_sorted


def _scale_from_radii(xs, ys, zs, target_px=P90_TARGET_PX, percentile=0.90,
                       amplitude_fraction=ZOOM_AMPLITUDE_FRACTION):
    """Same rationale/algorithm as orbital_view.py's _scale_from_radii()
    (measure this preset's own p90 radius rather than assuming n alone
    determines it) -- one PC-only difference: also returns r_ref itself
    (the device version only needs the two scales it derives from r_ref,
    not r_ref by name), since OrbitalViewApp needs it for the bounding
    sphere/marker's radius.
    """
    count = len(xs)
    radii = sorted(math.sqrt(xs[i] * xs[i] + ys[i] * ys[i] + zs[i] * zs[i]) for i in range(count))
    idx = min(count - 1, int(percentile * (count - 1)))
    r_ref = radii[idx] if radii[idx] > 1e-6 else 1.0
    base_scale = target_px / r_ref
    return base_scale, base_scale * amplitude_fraction, r_ref


def _bisect_rank(sorted_values, value):
    """Identical to orbital_view.py's _bisect_rank() -- manual binary
    search (not the `bisect` module) purely so this stays a direct visual
    diff against the device version; CPython's `bisect` would work fine
    here if you'd rather use it.
    """
    lo = 0
    hi = len(sorted_values)
    while lo < hi:
        mid = (lo + hi) // 2
        if sorted_values[mid] < value:
            lo = mid + 1
        else:
            hi = mid
    return lo


class ResampleState:
    """Identical in spirit to orbital_view.py's _ResampleState -- bundles
    what _resample_points() needs to keep drawing from the same
    distribution the initial cloud came from.
    """

    def __init__(self, sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted):
        self.sampler = sampler
        self.rng = rng
        self.radial_coeff = radial_coeff
        self.legendre_coeff = legendre_coeff
        self.n = n
        self.ell = ell
        self.m = m
        self.psi2_sorted = psi2_sorted
        self.cursor = 0


def resample_points(state, xs, ys, zs, colors, count):
    """Identical algorithm to orbital_view.py's _resample_points() -- see
    that function's docstring. Mutates xs/ys/zs (array.array('f'), plain
    float here -- no Q8 fixed-point conversion needed on the PC) and
    colors (the plain (r,g,b)-tuple list from point_colors()) in place.
    """
    n = len(xs)
    sampler = state.sampler
    rng = state.rng
    radial_coeff = state.radial_coeff
    legendre_coeff = state.legendre_coeff
    quantum_n, quantum_ell, quantum_m = state.n, state.ell, state.m
    psi2_sorted = state.psi2_sorted
    sorted_span = len(psi2_sorted) - 1 if len(psi2_sorted) > 1 else 1
    level_span = COLOR_MAX_LEVEL - COLOR_MIN_LEVEL

    for _ in range(count):
        idx = state.cursor
        state.cursor = idx + 1 if idx + 1 < n else 0

        x, y, z = pointcloud.sample_orbital_point(sampler, rng)
        xs[idx] = x
        ys[idx] = y
        zs[idx] = z

        r = math.sqrt(x * x + y * y + z * z)
        theta = math.acos(z / r) if r > 1e-9 else 0.0
        phi = math.atan2(y, x)
        psi = orbitals.psi_real(r, theta, phi, quantum_n, quantum_ell, quantum_m,
                                 radial_coeff, legendre_coeff)
        psi2 = psi * psi

        rank = _bisect_rank(psi2_sorted, psi2)
        level = COLOR_MIN_LEVEL + (rank * level_span) // sorted_span
        if level > COLOR_MAX_LEVEL:
            level = COLOR_MAX_LEVEL
        colors[idx] = _level_to_rgb(level)


def title_for_preset(preset):
    n, ell, m, label = preset
    return "%s (n=%d l=%d m=%d)" % (label, n, ell, m)


def load_preset(index):
    """PC equivalent of orbital_view.py's _load_preset() -- builds the
    cloud/colors/scale/resample-state for ORBITAL_PRESETS[index]. No Q8
    fixed-point conversion (see module docstring) and no serial print
    timing (this isn't measuring an ESP32's interpreted-Python cost) --
    otherwise the same steps in the same order.
    """
    n, ell, m, label = ORBITAL_PRESETS[index]
    print("orbital: loading preset %d (%s, n=%d l=%d m=%d)..." % (index, label, n, ell, m))
    t0 = time.time()
    xs, ys, zs, psi2, sampler, rng, radial_coeff, legendre_coeff = build_point_cloud(n, ell, m)
    colors, psi2_sorted = point_colors(psi2)
    title = title_for_preset(ORBITAL_PRESETS[index])
    base_scale, zoom_amplitude, r_ref = _scale_from_radii(xs, ys, zs)
    state = ResampleState(sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted)
    print("orbital: %s loaded in %.2fs, scale=%.1f" % (label, time.time() - t0, base_scale))
    return xs, ys, zs, colors, title, base_scale, zoom_amplitude, r_ref, state


def render_frame(buf, xs, ys, zs, colors, angle, scale, buzz_fraction=0.0):
    """PC equivalent of orbital_view.py's _render_frame()/_render_points():
    clear `buf` (a WIDTH*HEIGHT*3 RGB bytearray), draw the proton marker,
    then rotate+project+draw every point -- plain float math (see module
    docstring for why no fixed-point/viper is needed here), direct buffer
    writes (buf[idx:idx+3] = rgb, analogous to the device's ptr16 writes
    but simpler since there's no RGB565 packing to do).

    No 180-degree prism-offset flip -- see module docstring.
    """
    buf[:] = bytes(len(buf))  # fast bulk clear, not a per-byte Python loop

    proton_x0 = CENTER - PROTON_SIZE // 2
    proton_y0 = CENTER - PROTON_SIZE // 2
    pr, pg, pb = PROTON_COLOR
    for py in range(proton_y0, proton_y0 + PROTON_SIZE):
        if 0 <= py < HEIGHT:
            for px in range(proton_x0, proton_x0 + PROTON_SIZE):
                if 0 <= px < WIDTH:
                    idx = (py * WIDTH + px) * 3
                    buf[idx] = pr
                    buf[idx + 1] = pg
                    buf[idx + 2] = pb

    c = math.cos(angle)
    s = math.sin(angle)
    n = len(xs)
    for i in range(n):
        if buzz_fraction and random.random() < buzz_fraction:
            continue

        x = xs[i]
        y = ys[i]
        z = zs[i]

        rx = x * c + z * s
        sx = CENTER + int(rx * scale)
        sy = CENTER - int(y * scale)

        if 0 <= sx < WIDTH and 0 <= sy < HEIGHT:
            idx = (sy * WIDTH + sx) * 3
            r, g, b = colors[i]
            buf[idx] = r
            buf[idx + 1] = g
            buf[idx + 2] = b


class OrbitalViewApp:
    """tkinter app driving render_frame() -- the PC equivalent of
    orbital_view.py's run(), restructured around tkinter's non-blocking
    `.after()` scheduling instead of a blocking `while True` loop (a real
    `while True` would freeze the Tk event loop and the window would never
    repaint or receive key events).
    """

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("Orbital viewer -- PC debug (arrow keys = nudge, close window to quit)")

        self.canvas = tk.Canvas(self.root, width=DISPLAY_SIZE[0], height=DISPLAY_SIZE[1],
                                 bg='black', highlightthickness=0)
        self.canvas.pack()
        self.canvas.focus_set()

        tk.Label(self.root, text="Arrow keys = nudge (switch orbital). Close window to quit.",
                 fg='white', bg='black').pack(fill='x')

        self.imu = KeyboardIMU(self.canvas)
        self.detector = nudge.NudgeDetector(self.imu)

        self.buf = bytearray(WIDTH * HEIGHT * 3)
        self.photo = None  # kept alive on self; tkinter drops PhotoImages with no live reference
        self.image_id = self.canvas.create_image(0, 0, anchor='nw')

        self.preset_index = DEFAULT_PRESET_INDEX
        (self.xs, self.ys, self.zs, self.colors, self.title_text,
         self.base_scale, self.zoom_amplitude, self.r_ref, self.resample_state) = \
            load_preset(self.preset_index)

        self.angle = 0.0
        self.zoom_angle = 0.0
        self.two_pi = 2 * math.pi
        self.cull_count = max(1, int(len(self.xs) * CULL_FRACTION))
        self.cull_frame_count = 0

        self._fly_over(INTRO_START_SCALE_FACTOR, INTRO_FRAMES)

        self.root.after(0, self._tick)

    def run(self):
        self.root.mainloop()

    def _blit(self, scale, extra_text=None):
        image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(self.buf), 'raw', 'RGB', 0, 1)
        draw = ImageDraw.Draw(image)

        self._draw_bounding_sphere_and_marker(draw, scale)

        draw.text((2, 2), self.title_text, fill=(255, 255, 255))
        if extra_text:
            draw.text((2, 12), extra_text, fill=(255, 255, 255))
        image = image.resize(DISPLAY_SIZE, Image.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.canvas.itemconfig(self.image_id, image=self.photo)

    def _draw_bounding_sphere_and_marker(self, draw, scale):
        """See BOUNDING_SPHERE_COLOR's comment for the rationale. Drawn via
        PIL's ImageDraw on the small 240x240 image (same space as the point
        cloud and text -- see _blit()), rotated by the identical `angle`
        the cloud itself is rotated by, so the marker always shows the
        *current* frame's orientation, not a stale one.
        """
        px_r = self.r_ref * scale
        draw.ellipse((CENTER - px_r, CENTER - px_r, CENTER + px_r, CENTER + px_r),
                     outline=BOUNDING_SPHERE_COLOR)

        # Reference vector: (horizontal_r, y0, 0) before rotation --
        # elevated near the pole (see MARKER_ELEVATION_DEG) instead of
        # lying flat in the X/Z plane. Only the X/Z part rotates (same
        # rx = x*c + z*s as every sampled point in render_frame()); y0 is
        # fixed, keeping the marker consistently near the top regardless
        # of angle. Recomputed from self.r_ref every call rather than
        # cached on self -- one cos/sin pair per frame is trivial, and it
        # avoids a second place that needs updating in sync whenever
        # self.r_ref changes (every preset switch).
        horizontal_r = self.r_ref * math.cos(_MARKER_ELEVATION_RAD)
        y0 = self.r_ref * math.sin(_MARKER_ELEVATION_RAD)
        c = math.cos(self.angle)
        s = math.sin(self.angle)
        rx = horizontal_r * c
        rz = -horizontal_r * s  # not used by render_frame()'s points (ortho projection doesn't
                                 # need it there), but this reference vector needs it for the
                                 # toward/away-from-viewer depth cue below
        marker_x = CENTER + rx * scale
        marker_y = CENTER - y0 * scale  # subtract: screen y grows downward, same convention
                                         # render_frame() uses for points (sy = CENTER - int(y*scale))

        # depth_frac: 0.0 when the marker is rotating maximally away from
        # the viewer, 1.0 when maximally toward -- the only depth signal an
        # orthographic projection can give for motion confined to a
        # horizontal chord, so used to modulate brightness as a substitute
        # for the perspective/occlusion cues a true 3D renderer would have.
        depth_frac = (rz / horizontal_r + 1.0) / 2.0 if horizontal_r > 1e-6 else 0.5
        brightness = int(MARKER_COLOR_DIM + depth_frac * (MARKER_COLOR_BRIGHT - MARKER_COLOR_DIM))
        marker_color = (brightness, brightness, MARKER_BLUE)

        draw.text((marker_x, marker_y), MARKER_TEXT, fill=marker_color, font=_MARKER_FONT, anchor='mm')

    def _fly_over(self, start_factor, frames):
        """PC equivalent of orbital_view.py's _fly_over() -- blocking (uses
        self.root.update() per frame to keep the window responsive during
        the transition) since it's a short, one-shot camera move, same as
        the device version being a plain sub-loop inside run().
        """
        start_scale = self.base_scale * start_factor
        end_scale = self.base_scale
        for i in range(frames):
            t = i / (frames - 1) if frames > 1 else 1.0
            scale = start_scale + (end_scale - start_scale) * t
            render_frame(self.buf, self.xs, self.ys, self.zs, self.colors, self.angle, scale)
            self._blit(scale)
            self.root.update()
            self.angle = (self.angle + ANGLE_STEP) % self.two_pi

    def _tick(self):
        if self.detector is not None:
            raw = self.detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = self.detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                step = _NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    self.preset_index = (self.preset_index + step) % len(ORBITAL_PRESETS)
                    (self.xs, self.ys, self.zs, self.colors, self.title_text,
                     self.base_scale, self.zoom_amplitude, self.r_ref, self.resample_state) = \
                        load_preset(self.preset_index)
                    self.cull_count = max(1, int(len(self.xs) * CULL_FRACTION))
                    self.cull_frame_count = 0
                    self._fly_over(SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES)

        self.cull_frame_count += 1
        if self.cull_frame_count >= CULL_REFRESH_FRAMES:
            resample_points(self.resample_state, self.xs, self.ys, self.zs, self.colors, self.cull_count)
            self.cull_frame_count = 0

        scale = self.base_scale + self.zoom_amplitude * math.sin(self.zoom_angle)
        render_frame(self.buf, self.xs, self.ys, self.zs, self.colors, self.angle, scale,
                     buzz_fraction=BUZZ_FRACTION)
        self._blit(scale)

        self.angle = (self.angle + ANGLE_STEP) % self.two_pi
        self.zoom_angle = (self.zoom_angle + ZOOM_ANGLE_STEP) % self.two_pi

        self.root.after(FRAME_DELAY_MS, self._tick)


def run():
    OrbitalViewApp().run()


if __name__ == '__main__':
    run()
