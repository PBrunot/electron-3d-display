"""Browser counterpart of pc/viewer_common.py: same camera model (yaw/tilt/
roll advance, intro/preset-switch fly-overs, random zoom excursions) and the
same shared zoom envelope (outer_bound_scale()/inner_bound_scale()/
shell_count_frames()) -- but built for Pyodide + an HTML5 <canvas> instead of
tkinter + PIL, run by requestAnimationFrame instead of a blocking loop.

Two things had to change shape, not just backend, to fit a browser's
single-threaded event loop:

  - fly_over()/maybe_zoom_excursion() used to BLOCK (a for-loop calling
    tkinter's root.update() each frame) -- that would freeze the browser tab
    solid for the whole sequence. Here they're GENERATORS instead
    (fly_over_gen()/zoom_excursion_gen()): each `yield` is one rendered
    frame, and the JS driver (see index.html) calls next() on whichever
    generator is active once per animation frame. web_atom.py's dissection
    sequence follows the same pattern.
  - render_frame()/draw_nucleus()/rotate_yaw_tilt_roll() needed no changes
    at all -- they only ever touched a plain RGB bytearray, never PIL, so
    they're copied here verbatim. Only the overlay drawing (bounding circle,
    rotating marker, scale bar) and the final blit had to be rewritten
    against the Canvas 2D API instead of PIL's ImageDraw/ImageTk.

WIDTH/HEIGHT/CENTER intentionally match pc/viewer_common.py's (480/480/240)
so every tuned camera constant below (INTRO_*, ZOOM_*) carries over from the
PC viewer unchanged -- no rescaling to re-derive or re-tune for a different
canvas size. N_POINTS is trimmed on the caller's side instead (see
web_atom.py) since Pyodide's interpreted per-point loop, not canvas
resolution, is the real per-frame cost in a browser tab.
"""

import math
import random

import numpy as np
import js
from pyodide.ffi import to_js

# --- Display geometry --------------------------------------------------------
WIDTH = 480
HEIGHT = 480
CENTER = WIDTH // 2

# --- Camera motion ------------------------------------------------------------
# Identical to pc/viewer_common.py -- see that module for the derivation.
ANGLE_STEP = 0.030
TILT_ANGLE_STEP = 0.023
ROLL_ANGLE_STEP = 0.017
ZOOM_ANGLE_STEP = 0.016
_TILT_ANGLE_START = 0.9
_ROLL_ANGLE_START = 2.1
FRAME_DELAY_MS = 20  # target frame interval; index.html's rAF loop throttles to this

# --- Intro / preset-switch transitions ----------------------------------------
INTRO_START_SCALE_FACTOR = 1.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 4.0
SWITCH_TRANSITION_FRAMES = 20

# --- Zoom bounds shared by the excursion dive and the atom dissection --------
ZOOM_BOUNDS_TARGET_PX = 150.0
ZOOM_OUTER_RADIUS_FACTOR = 1.25
ZOOM_INNER_RADIUS_FACTOR = 0.35


def outer_bound_scale(outer_r_ref, scale_factor=1.0):
    return scale_factor * ZOOM_BOUNDS_TARGET_PX / max(outer_r_ref * ZOOM_OUTER_RADIUS_FACTOR, 1e-6)


def inner_bound_scale(inner_r_ref, scale_factor=1.0):
    return scale_factor * ZOOM_BOUNDS_TARGET_PX / max(inner_r_ref * ZOOM_INNER_RADIUS_FACTOR, 1e-6)


def shell_count_frames(base_frames, per_shell_frames, shell_count):
    return int(base_frames + per_shell_frames * max(0, shell_count - 1))


# --- Random zoom excursions ---------------------------------------------------
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 500
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 1000
ZOOM_EXCURSION_EASE_FRAMES_BASE = 100
ZOOM_EXCURSION_EASE_FRAMES_PER_SHELL = 50

# --- Bounding sphere + rotation marker -----------------------------------------
BOUNDING_SPHERE_COLOR = (70, 70, 90)
MARKER_FONT_PX = 15
MARKER_ELEVATION_DEG = 50.0
_MARKER_ELEVATION_RAD = math.radians(MARKER_ELEVATION_DEG)
MARKER_COLOR_BEHIND = (110, 110, 110)
MARKER_COLOR_FRONT = (255, 220, 40)

# --- Nucleus -------------------------------------------------------------------
PROTON_SIZE = 3
PROTON_COLOR = (255, 0, 0)

# --- Electron point rendering ---------------------------------------------------
ELECTRON_ALPHA = 0.8

ENABLE_PERSISTENCE = True
PERSISTENCE_DECAY = 100
_PERSISTENCE_TABLE = bytes((i * PERSISTENCE_DECAY) // 256 for i in range(256))

# --- Scale bar -------------------------------------------------------------------
SCALE_BAR_MARGIN_X = 8
SCALE_BAR_MARGIN_Y = 8
SCALE_BAR_MAX_PX = 90
SCALE_BAR_TICK_PX = 4
SCALE_BAR_COLOR = (210, 210, 210)
SCALE_BAR_FONT_PX = 12

# --- HUD text positions -----------------------------------------------------------
TITLE_POS = (4, 4)
SUBTITLE_POS = (4, 20)
TITLE_FONT_PX = 15


def next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)


def draw_nucleus(buf):
    """Verbatim copy of pc/viewer_common.py's draw_nucleus() -- pure
    bytearray writes, no PIL involved there either.
    """
    proton_x0 = CENTER - PROTON_SIZE // 2
    proton_y0 = CENTER - PROTON_SIZE // 2
    pr, pg, pb = PROTON_COLOR
    for py in range(proton_y0, proton_y0 + PROTON_SIZE):
        if 0 <= py < HEIGHT:
            for px in range(proton_x0, proton_x0 + PROTON_SIZE):
                if 0 <= px < WIDTH:
                    idx = (py * WIDTH + px) * 3
                    buf[idx], buf[idx + 1], buf[idx + 2] = pr, pg, pb


def rotate_yaw_tilt_roll(x, y, z, cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll):
    """Verbatim copy of pc/viewer_common.py's rotate_yaw_tilt_roll()."""
    rx1 = x * cos_yaw + z * sin_yaw
    rz1 = z * cos_yaw - x * sin_yaw
    ry2 = y * cos_tilt - rz1 * sin_tilt
    rz = y * sin_tilt + rz1 * cos_tilt
    rx3 = rx1 * cos_roll - ry2 * sin_roll
    ry3 = rx1 * sin_roll + ry2 * cos_roll
    return rx3, ry3, rz


def render_frame(buf, preset, angle, tilt_angle, roll_angle, scale, buzz_fraction=0.0):
    """Verbatim copy of pc/viewer_common.py's render_frame()."""
    if ENABLE_PERSISTENCE:
        buf[:] = buf.translate(_PERSISTENCE_TABLE)
    else:
        buf[:] = bytes(len(buf))

    draw_nucleus(buf)

    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    xs, ys, zs, colors = preset.xs, preset.ys, preset.zs, preset.colors
    for i in range(len(xs)):
        if buzz_fraction and random.random() < buzz_fraction:
            continue

        rx3, ry3, _rz = rotate_yaw_tilt_roll(xs[i], ys[i], zs[i],
                                             cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll)
        px = CENTER + round(rx3 * scale)
        py = CENTER - round(ry3 * scale)

        if 0 <= px < WIDTH and 0 <= py < HEIGHT:
            idx = (py * WIDTH + px) * 3
            cr, cg, cb = colors[i]
            buf[idx] = buf[idx] + int((cr - buf[idx]) * ELECTRON_ALPHA)
            buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * ELECTRON_ALPHA)
            buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * ELECTRON_ALPHA)


def advance_rotation(app):
    app.angle = (app.angle + ANGLE_STEP) % app.two_pi
    app.tilt_angle = (app.tilt_angle + TILT_ANGLE_STEP) % app.two_pi
    app.roll_angle = (app.roll_angle + ROLL_ANGLE_STEP) % app.two_pi


def fly_over_gen(app, start_scale, end_scale, frames):
    """Generator version of pc/viewer_common.py's fly_over(): each `yield`
    is one rendered frame, driven by index.html's requestAnimationFrame loop
    instead of a blocking for-loop + root.update(). Takes absolute scales so
    it can ease to/from anywhere, not just back to base_scale.
    """
    for i in range(frames):
        t = i / (frames - 1) if frames > 1 else 1.0
        scale = start_scale + (end_scale - start_scale) * t
        render_frame(app.buf, app.preset, app.angle, app.tilt_angle, app.roll_angle, scale)
        app.blit(scale)
        advance_rotation(app)
        yield


def zoom_excursion_gen(app, base_scale, zoom_amplitude, outer_r_ref, inner_r_ref,
                        shell_count=1, scale_factor=1.0):
    """Generator version of pc/viewer_common.py's maybe_zoom_excursion(),
    minus the countdown gate (the caller checks app.zoom_excursion_countdown
    itself and only starts this generator once it's due -- see
    web_atom.py's WebAtomApp.tick()). Dives from wherever the camera
    currently is out to the shared "outside" bound, in through the cloud to
    the shared "deep" bound, and back to the resting breathing scale. See
    pc/viewer_common.py's maybe_zoom_excursion() docstring for the full
    rationale; the bounds/pacing math is identical.
    """
    current_scale = base_scale + zoom_amplitude * math.sin(app.zoom_angle)
    outer_scale = outer_bound_scale(outer_r_ref, scale_factor)
    inner_scale = inner_bound_scale(inner_r_ref, scale_factor)
    frames = shell_count_frames(ZOOM_EXCURSION_EASE_FRAMES_BASE, ZOOM_EXCURSION_EASE_FRAMES_PER_SHELL, shell_count)
    yield from fly_over_gen(app, current_scale, outer_scale, frames)
    yield from fly_over_gen(app, outer_scale, inner_scale, frames)
    yield from fly_over_gen(app, inner_scale, base_scale, frames)
    app.zoom_angle = 0.0
    app.zoom_excursion_countdown = next_zoom_excursion_countdown()


# --- Canvas backend ------------------------------------------------------------
# Everything below replaces pc/viewer_common.py's PIL/tkinter blit
# (blit_to_canvas(), draw_bounding_circle(), draw_orbit_marker(),
# draw_scale_bar()) with the Canvas 2D API, reached via Pyodide's `js`
# bridge. bind_canvas() must run once (see web_atom.py's WebAtomApp.start())
# before blit_buf()/draw_*_canvas() are called.
_ctx = None
_image_data = None


def bind_canvas(canvas_id):
    """Look up the <canvas> element, size it to WIDTH x HEIGHT (the internal
    math resolution -- CSS handles any responsive display scaling, see
    index.html), and cache a reusable ImageData buffer so blit_buf() doesn't
    allocate one every frame.
    """
    global _ctx, _image_data
    canvas = js.document.getElementById(canvas_id)
    canvas.width = WIDTH
    canvas.height = HEIGHT
    _ctx = canvas.getContext('2d')
    _image_data = _ctx.createImageData(WIDTH, HEIGHT)
    return _ctx


def rgb_css(color):
    r, g, b = color
    return "rgb(%d,%d,%d)" % (r, g, b)


def blit_buf(buf):
    """Push `buf` (a WIDTH*HEIGHT*3 RGB bytearray, see render_frame()) to the
    canvas. buf has no alpha channel (persistence fading relies on plain RGB
    -- see render_frame()'s ENABLE_PERSISTENCE comment), so this expands it
    to RGBA with numpy (fast enough to redo every frame; a pure-Python
    per-pixel loop over WIDTH*HEIGHT=230400 pixels was the alternative and
    would be the actual bottleneck, not the point-rendering loop above).
    """
    rgb = np.frombuffer(buf, dtype=np.uint8).reshape(HEIGHT, WIDTH, 3)
    rgba = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
    rgba[:, :, :3] = rgb
    rgba[:, :, 3] = 255
    js_bytes = js.Uint8Array.new(to_js(rgba.tobytes()))
    _image_data.data.set(js_bytes)
    _ctx.putImageData(_image_data, 0, 0)


def draw_bounding_circle_canvas(r_ref, scale, outline_color=BOUNDING_SPHERE_COLOR):
    """Canvas counterpart of pc/viewer_common.py's draw_bounding_circle()."""
    px_r = max(r_ref * scale, 0)
    _ctx.strokeStyle = rgb_css(outline_color)
    _ctx.beginPath()
    _ctx.arc(CENTER, CENTER, px_r, 0, 2 * math.pi)
    _ctx.stroke()


def draw_orbit_marker_canvas(r_ref, scale, angle, tilt_angle, roll_angle, marker_text,
                              outline_color=BOUNDING_SPHERE_COLOR):
    """Canvas counterpart of pc/viewer_common.py's draw_orbit_marker()."""
    draw_bounding_circle_canvas(r_ref, scale, outline_color)

    horizontal_r = r_ref * math.cos(_MARKER_ELEVATION_RAD)
    y0 = r_ref * math.sin(_MARKER_ELEVATION_RAD)
    cos_yaw, sin_yaw = math.cos(angle), math.sin(angle)
    cos_tilt, sin_tilt = math.cos(tilt_angle), math.sin(tilt_angle)
    cos_roll, sin_roll = math.cos(roll_angle), math.sin(roll_angle)
    rx3, ry3, rz = rotate_yaw_tilt_roll(horizontal_r, y0, 0.0,
                                        cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll)
    marker_x = CENTER + rx3 * scale
    marker_y = CENTER - ry3 * scale

    depth_frac = (rz / r_ref + 1.0) / 2.0 if r_ref > 1e-6 else 0.5
    marker_color = tuple(
        int(MARKER_COLOR_BEHIND[c] + depth_frac * (MARKER_COLOR_FRONT[c] - MARKER_COLOR_BEHIND[c]))
        for c in range(3))
    css = rgb_css(marker_color)

    _ctx.strokeStyle = css
    _ctx.beginPath()
    _ctx.moveTo(CENTER, CENTER)
    _ctx.lineTo(marker_x, marker_y)
    _ctx.stroke()

    _ctx.fillStyle = css
    _ctx.font = "%dpx sans-serif" % MARKER_FONT_PX
    _ctx.textAlign = "center"
    _ctx.textBaseline = "middle"
    _ctx.fillText(marker_text, marker_x, marker_y)


def draw_scale_bar_canvas(cloud_common_mod, pixels_per_unit, unit_label,
                           canvas_height=HEIGHT, max_bar_px=SCALE_BAR_MAX_PX):
    """Canvas counterpart of pc/viewer_common.py's draw_scale_bar(). Takes
    the cloud_common module explicitly (rather than importing it here) so
    this module has no hard dependency on the model layer.
    """
    if pixels_per_unit <= 0:
        return
    length, label = cloud_common_mod.pick_scale_bar_length(pixels_per_unit, max_bar_px)
    bar_px = length * pixels_per_unit

    x0 = SCALE_BAR_MARGIN_X
    y = canvas_height - SCALE_BAR_MARGIN_Y
    x1 = x0 + bar_px

    css = rgb_css(SCALE_BAR_COLOR)
    _ctx.strokeStyle = css
    _ctx.beginPath()
    _ctx.moveTo(x0, y)
    _ctx.lineTo(x1, y)
    _ctx.moveTo(x0, y - SCALE_BAR_TICK_PX)
    _ctx.lineTo(x0, y + SCALE_BAR_TICK_PX)
    _ctx.moveTo(x1, y - SCALE_BAR_TICK_PX)
    _ctx.lineTo(x1, y + SCALE_BAR_TICK_PX)
    _ctx.stroke()

    _ctx.fillStyle = css
    _ctx.font = "%dpx sans-serif" % SCALE_BAR_FONT_PX
    _ctx.textAlign = "left"
    _ctx.textBaseline = "bottom"
    _ctx.fillText("%s %s" % (label, unit_label), x0, y - SCALE_BAR_TICK_PX - 2)


def draw_text_canvas(x, y, text, color, font_px=TITLE_FONT_PX, align='left', baseline='top'):
    """Plain single-color text overlay -- dissection labels, the Z=n note,
    web_atom.py's multi-color title helper (one segment at a time), and
    web_app.py's chooser button/title labels (align='center').
    """
    _ctx.font = "%dpx sans-serif" % font_px
    _ctx.textAlign = align
    _ctx.textBaseline = baseline
    _ctx.fillStyle = rgb_css(color)
    _ctx.fillText(text, x, y)


def measure_text_canvas(text, font_px=TITLE_FONT_PX):
    _ctx.font = "%dpx sans-serif" % font_px
    return _ctx.measureText(text).width


def draw_rect_canvas(x0, y0, x1, y1, fill_color=None, outline_color=None, line_width=2):
    """Filled and/or outlined rectangle -- web_app.py's chooser buttons
    (tkinter Canvas items on PC; the 2D canvas API has no persistent
    "items" to reuse, so this is redrawn every frame like everything else).
    """
    if fill_color is not None:
        _ctx.fillStyle = rgb_css(fill_color)
        _ctx.fillRect(x0, y0, x1 - x0, y1 - y0)
    if outline_color is not None:
        _ctx.strokeStyle = rgb_css(outline_color)
        _ctx.lineWidth = line_width
        _ctx.strokeRect(x0, y0, x1 - x0, y1 - y0)
