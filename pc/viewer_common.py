"""Render/camera helpers shared by pc/orbital_view_pc.py and
pc/atom_view_pc.py. Both viewers show a tumbling point cloud in a tkinter
window with the same camera model (yaw/tilt/roll advance, intro/preset-switch
fly-overs, random zoom excursions), the same nucleus/marker/scale-bar
overlays, and the same buf->PIL->canvas blit -- this module is that common
layer, extracted so atom_view_pc.py imports a plain shared module instead of
reaching into orbital_view_pc.py's internals (an "app" module) for it.

What stays OUT of this module, in each viewer instead: the per-viewer Preset
class (Preset/AtomPreset -- different data sources, cloud_common.Preset's
point-turnover vs AtomPreset's static cloud), N_POINTS (different budgets),
the tkinter App class and its input handling (nudge gesture vs Up/Down keys),
and DEBUG_DISABLE_*/buzz (orbital-only debug switches).
"""

import math
import os
import random
import sys

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import cloud_common

from PIL import Image, ImageDraw, ImageFont, ImageTk

# numpy is optional: when present, render_frame()/render_dissection_frame()
# take a fully vectorized fast path (the per-point Python loop was the whole
# reason the PC ran at ~7 fps); the pure-Python paths remain as the fallback
# so the viewers still run on a numpy-less install. The vectorized core
# itself lives in render_core.py, SHARED with the web port (web/py/web_common
# imports the same module under Pyodide) so the two can't drift apart.
import render_core

_HAS_NUMPY = render_core._HAS_NUMPY
_preset_np = render_core.preset_np

# --- Display geometry -------------------------------------------------------
WIDTH = 480
HEIGHT = 480
CENTER = WIDTH // 2
DISPLAY_SCALE = 1  # tkinter window is WIDTH*DISPLAY_SCALE square; math stays at WIDTH/HEIGHT
DISPLAY_SIZE = (WIDTH * DISPLAY_SCALE, HEIGHT * DISPLAY_SCALE)

# --- Camera motion ----------------------------------------------------------
# Yaw/tilt/roll angular speed per frame. Roll is required, not cosmetic:
# yaw+tilt alone leave a point's screen-X independent of tilt, pinning points
# near the world Y axis to the vertical screen centerline (see orbital_view.py's
# module docstring for the derivation). Tilt/roll are kept close to ANGLE_STEP
# (and non-resonant with it) so axis-aligned lobes visibly rotate instead of
# lagging behind yaw, and the tumble doesn't fall into a short repeating loop.
ANGLE_STEP = 0.030
TILT_ANGLE_STEP = 0.023
ROLL_ANGLE_STEP = 0.017
ZOOM_ANGLE_STEP = 0.016
# Tilt/roll start away from the degenerate all-zero pose (where yaw alone
# can't move axis-aligned lobes at all), so even frame 0 isn't axis-locked.
_TILT_ANGLE_START = 0.9
_ROLL_ANGLE_START = 2.1
# Throttle between _tick() frames. With the numpy fast path the render
# itself is a few ms, so this delay is a small pacing idle, not the
# bottleneck -- 5ms leaves the loop at ~30-40 fps, matching the ESP32.
FRAME_DELAY_MS = 5

# --- Intro / preset-switch transitions --------------------------------------
INTRO_START_SCALE_FACTOR = 1
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 3.0
SWITCH_TRANSITION_FRAMES = 20

# --- Zoom bounds shared by the excursion dive and the atom dissection -------
# Both animations guarantee the same physical span: eased out to twice the
# cloud's own outer radius (a clearly "outside" overview) and eased in to a
# QUARTER of the innermost/first shell's own radius (well past that shell's
# own extent, so the dive clearly passes into/through it), then back. TARGET_PX
# matches cloud_common.P90_TARGET_PX/the dissection's own on-screen framing
# target, so "radius R fills TARGET_PX" means the same thing everywhere.
ZOOM_BOUNDS_TARGET_PX = 350.0
ZOOM_OUTER_RADIUS_FACTOR = 1.25   # outer bound: outer radius x2 fills TARGET_PX (zoomed OUT)
ZOOM_INNER_RADIUS_FACTOR = 0.35  # inner bound: first-shell radius x0.25 fills TARGET_PX (zoomed IN)


def outer_bound_scale(outer_r_ref, scale_factor=1.0):
    """Scale at which `outer_r_ref` (the cloud's own outer/valence radius)
    x ZOOM_OUTER_RADIUS_FACTOR fills ZOOM_BOUNDS_TARGET_PX -- the "outside"
    end of the shared zoom envelope. `scale_factor` folds in a caller's
    manual zoom (e.g. atom_view_pc.py's mouse-wheel zoom_factor) so the
    bound stays relative to wherever the user has zoomed to; pass 1.0 where
    no manual zoom applies (e.g. the dissection sequence, whose per-shell
    targets already ignore it -- see _run_dissection()).
    """
    return scale_factor * ZOOM_BOUNDS_TARGET_PX / max(outer_r_ref * ZOOM_OUTER_RADIUS_FACTOR, 1e-6)


def inner_bound_scale(inner_r_ref, scale_factor=1.0):
    """Scale at which `inner_r_ref` (the first/innermost shell's own radius)
    x ZOOM_INNER_RADIUS_FACTOR fills ZOOM_BOUNDS_TARGET_PX -- the "deep dive"
    end of the shared zoom envelope. See outer_bound_scale() for
    `scale_factor`.
    """
    return scale_factor * ZOOM_BOUNDS_TARGET_PX / max(inner_r_ref * ZOOM_INNER_RADIUS_FACTOR, 1e-6)


def shell_count_frames(base_frames, per_shell_frames, shell_count):
    """Ease-leg frame count that grows with `shell_count` -- a heavier,
    multi-shell atom's dive/dissection gets proportionally more frames per
    leg so it doesn't feel rushed next to a single-shell (shell_count=1)
    hydrogen orbital, which gets exactly `base_frames`.
    """
    return int(base_frames + per_shell_frames * max(0, shell_count - 1))


# --- Random zoom excursions -------------------------------------------------
# At randomized intervals, dive from the current breathing scale out to the
# shared "outside" bound, in through the cloud to the shared "deep" bound,
# and back to the resting breathing scale, layered on the constant sine
# breathing so the motion doesn't read as purely mechanical. No render
# budget to protect on PC, so a dive can feel like passing through the cloud
# itself.
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 500
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 1000
ZOOM_EXCURSION_EASE_FRAMES_BASE = 100
ZOOM_EXCURSION_EASE_FRAMES_PER_SHELL = 50

# --- Bounding sphere + rotation marker ---------------------------------------
BOUNDING_SPHERE_COLOR = (70, 70, 90)
MARKER_TEXT = "H"
MARKER_FONT_SIZE = 15
# Elevated near the pole (not 0deg, which would sit exactly on the Y rotation
# axis and never move) so the marker visibly moves every frame, giving an
# unambiguous read on rotation direction/speed.
MARKER_ELEVATION_DEG = 50.0
_MARKER_ELEVATION_RAD = math.radians(MARKER_ELEVATION_DEG)
MARKER_COLOR_BEHIND = (110, 110, 110)  # rotating away from the viewer
MARKER_COLOR_FRONT = (255, 220, 40)    # rotating toward the viewer -- a warm
                                        # color shift reads much stronger than
                                        # a gray brightness change
_MARKER_FONT = ImageFont.load_default(size=MARKER_FONT_SIZE)  # loaded once, not per frame

# --- Nucleus ----------------------------------------------------------------
# 14px, not the device's 7: the PC buffer is 480x480 = 2x the 240 panel, so 2x
# the panel px gives the same relative on-screen size. Matches today's device
# change (src/views/orbital_view.cpp's kOrbitalProtonMarkerSize / atom_view.cpp's
# kAtomProtonMarkerSize 3->7, "proton not visible enough, give him a bigger
# radius"). Drawn AFTER the cloud (see render_frame()) so it's always a fully
# opaque bright-red point on top and can't be dimmed out by points landing on
# the same pixels -- the same on-top redraw the device now does every frame.
PROTON_SIZE = 4
PROTON_COLOR = (255, 0, 0)

# --- Electron point rendering ------------------------------------------------
# Each point alpha-blends toward its own color instead of overwriting the
# pixel (1.0 = opaque). Overlapping points converge toward full brightness,
# so apparent brightness tracks local sample DENSITY at a pixel -- the way a
# translucent point cloud reads. The nucleus above is NOT blended (one literal
# particle, not a probability cloud).
# Raised from 0.8 to ~0.92 to match today's device-side change
# (src/render/camera.h's kElectronAlphaQ8 205->235): during rotation a given point
# rarely lands on the exact same pixel two frames running, so it gets
# essentially one blend toward full brightness before the persistence fade
# below starts pulling that pixel back down -- the cloud reads visibly dimmer
# in motion than in a static single-frame render. A stronger alpha makes each
# individual hit closer to full brightness, which is where the perceived
# dimming during animation/rotation actually comes from.
ELECTRON_ALPHA = 0.92
# Each electron renders as an ELECTRON_SIZE x ELECTRON_SIZE square block
# (1 = single pixel, the old behavior). 2 = double size: the PC buffer is
# 480x480 = 2x the device's 240x240 panel, so a 2x2 block here is exactly one
# device pixel -- the PC preview then shows electrons at the same apparent
# size as the panel, instead of half-size dots.
ELECTRON_SIZE = 1

# Phosphor-style persistence (PC-only cosmetic; the device hard-clears each
# frame -- see orbital_view.py). Each frame fades the previous buffer toward
# black via bytes.translate() (one C-level pass, effectively free at
# 240x240x3 bytes/frame) instead of clearing, so points leave a trailing glow
# and skipped "buzz" points fade out instead of vanishing.
ENABLE_PERSISTENCE = True
# Matches the device's kPersistenceKeepQ8 in spirit, not value: slower decay
# keeps a hit pixel's glow alive longer between re-hits as points sweep
# across the screen during rotation, filling in the gaps that otherwise read
# as fading -- but at the numpy fast path's ~30 fps, a given decay value
# spans roughly half as many wall-clock seconds as it would at half that
# frame rate, so 120 (~0.47 kept/frame) reads as the right trail length here
# where the device's own value would read as too long.
PERSISTENCE_DECAY = 120  # /256 kept per frame (~0.47); lower = shorter trails, 256 = never fades
_PERSISTENCE_TABLE = bytes((i * PERSISTENCE_DECAY) // 256 for i in range(256))

# --- Scale bar (bottom-left physical-size reference) ------------------------
# "Nice" round lengths + the picking rule live in cloud_common.py
# (pick_scale_bar_length()), shared with the device renderer so a bar reads
# the same physical length on both. What's left here is PIL-specific geometry.
# Every dimension doubled to match today's device-side change
# (src/render/overlay.cpp: "la scaletta risulta illegibile, raddoppia le sue
# dimensioni font compresa") -- margins, tick height, bar line thickness, and
# the label now at a 2x font instead of PIL's tiny default.
SCALE_BAR_MARGIN_X = 16
SCALE_BAR_MARGIN_Y = 16
SCALE_BAR_MAX_PX = 180
SCALE_BAR_TICK_PX = 8
SCALE_BAR_LINE_WIDTH = 2   # was an implicit 1px line
SCALE_BAR_FONT_SIZE = 22   # ~2x the old default bitmap font (~11px)
SCALE_BAR_LABEL_GAP_PX = 4  # px between label bottom and the tick top
SCALE_BAR_COLOR = (210, 210, 210)
_SCALE_BAR_FONT = ImageFont.load_default(size=SCALE_BAR_FONT_SIZE)  # loaded once, not per frame

# --- HUD text positions -------------------------------------------------------
TITLE_POS = (2, 2)
SUBTITLE_POS = (2, 12)


def _next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)


def find_unicode_font(size):
    """First installed TrueType font that can render Greek letters, from a
    cross-platform candidate list; None if none exist (callers then fall back
    to the default PIL font / ASCII text). Segoe UI and Arial ship with
    Windows; DejaVu/Liberation are the usual Linux desktop defaults. Used by
    both viewers' full-screen title/equation text.
    """
    for path in (
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    ):
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    return None


def draw_nucleus(buf):
    """Draw the fully-opaque nucleus marker (small square) at screen center.
    The nucleus is one literal particle, not a probability cloud, so it never
    alpha-blends.
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
    """Rotate (x, y, z) by yaw (about Y), tilt (about X), roll (about Z).
    Returns (rx, ry, rz). rz is the post-yaw-and-tilt depth -- roll (about Z)
    never changes z, so rz is the correct depth cue for clipping.
    """
    rx1 = x * cos_yaw + z * sin_yaw
    rz1 = z * cos_yaw - x * sin_yaw
    ry2 = y * cos_tilt - rz1 * sin_tilt
    rz = y * sin_tilt + rz1 * cos_tilt
    rx3 = rx1 * cos_roll - ry2 * sin_roll
    ry3 = rx1 * sin_roll + ry2 * cos_roll
    return rx3, ry3, rz


def blend_electron(buf, px, py, cr, cg, cb, alpha=ELECTRON_ALPHA, size=ELECTRON_SIZE):
    """Alpha-blend one electron into `buf` (a WIDTH*HEIGHT*3 RGB bytearray)
    as an `size` x `size` square block centered on (px, py), clipped at the
    screen edges. Every block pixel blends exactly the way the old single
    point did, so overlapping blocks still converge toward full brightness
    and apparent brightness keeps tracking local sample density. Shared by
    render_frame() and atom_view_pc.render_dissection_frame()'s _draw() so
    both PC views draw electrons at the same size.

    Sizes 1 and 2 (the default) have unrolled fast paths -- this is called
    once per point per frame (20000+ calls), so the generic range()-loop
    version below would add measurable per-point overhead on top of the
    pixel writes themselves.
    """
    if size == 1:
        if 0 <= px < WIDTH and 0 <= py < HEIGHT:
            idx = (py * WIDTH + px) * 3
            buf[idx] = buf[idx] + int((cr - buf[idx]) * alpha)
            buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * alpha)
            buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * alpha)
        return

    if size == 2:
        x0, y0 = px - 1, py - 1
        for yy in (y0, y0 + 1):
            if 0 <= yy < HEIGHT:
                row = yy * WIDTH
                for xx in (x0, x0 + 1):
                    if 0 <= xx < WIDTH:
                        idx = (row + xx) * 3
                        buf[idx] = buf[idx] + int((cr - buf[idx]) * alpha)
                        buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * alpha)
                        buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * alpha)
        return

    # General path (uncommon sizes > 2): clip the block to the screen and
    # blend every in-bounds pixel.
    half = size // 2
    x0 = max(px - half, 0)
    y0 = max(py - half, 0)
    x1 = min(px - half + size, WIDTH)
    y1 = min(py - half + size, HEIGHT)
    for yy in range(y0, y1):
        row = yy * WIDTH
        for xx in range(x0, x1):
            idx = (row + xx) * 3
            buf[idx] = buf[idx] + int((cr - buf[idx]) * alpha)
            buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * alpha)
            buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * alpha)


def _blend_points_np(buf_np, xs, ys, zs, colors, cy, sy, cx, sx, cz, sz, scale,
                     buzz_fraction=0.0, clip_rz_max=None, skip_mask=None, alpha=ELECTRON_ALPHA):
    """Vectorized core of render_frame()/render_dissection_frame(), bound to
    this platform's geometry/style constants -- implementation shared with
    the web port lives in render_core.blend_points() (see that module's
    docstring for the exact sequential-blend semantics it preserves).
    """
    return render_core.blend_points(buf_np, xs, ys, zs, colors, cy, sy, cx, sx, cz, sz, scale,
                                    WIDTH, HEIGHT, CENTER, ELECTRON_SIZE, alpha,
                                    buzz_fraction, clip_rz_max, skip_mask)


def _draw_nucleus_np(buf_np):
    """Vectorized draw_nucleus(): the fully-opaque PROTON_SIZE square at
    screen center, written on top of the cloud (see render_frame())."""
    return render_core.draw_nucleus(buf_np, WIDTH, HEIGHT, CENTER, PROTON_SIZE, PROTON_COLOR)


def _render_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale, buzz_fraction=0.0):
    """numpy fast path of render_frame() -- see that function's docstring;
    mutates `buf` (the bytearray) in place via a zero-copy view."""
    render_core.render_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale,
                                WIDTH, HEIGHT, CENTER, ELECTRON_SIZE, ELECTRON_ALPHA,
                                PERSISTENCE_DECAY, PROTON_SIZE, PROTON_COLOR,
                                buzz_fraction=buzz_fraction, enable_persistence=ENABLE_PERSISTENCE)


def render_frame(buf, preset, angle, tilt_angle, roll_angle, scale, buzz_fraction=0.0):
    """Clear (or fade, see ENABLE_PERSISTENCE) `buf`, rotate (yaw/tilt/roll
    -- all three needed, see orbital_view.py's module docstring) and
    alpha-blend every point of `preset` into the buffer, then draw the
    nucleus LAST, fully opaque, on top of the cloud -- matching the device's
    today's change: the old order (nucleus first, points blending over it)
    let a point landing on the same pixel dim/hide it (worst near screen
    center for any orbital whose density peaks at the origin). `preset` need
    only expose xs/ys/zs/colors (Preset and AtomPreset both do).

    With numpy installed this takes the vectorized fast path (_render_frame_np
    -- ~10ms at 20000 points instead of ~140ms, the whole difference between
    7 and 30 fps); the pure-Python loop below is the no-numpy fallback.
    """
    if _HAS_NUMPY and ELECTRON_SIZE in (1, 2):
        arr = _preset_np(preset)
        if arr is not None:
            _render_frame_np(buf, preset, arr, angle, tilt_angle, roll_angle, scale, buzz_fraction)
            return

    if ENABLE_PERSISTENCE:
        buf[:] = buf.translate(_PERSISTENCE_TABLE)  # fade previous frame instead of clearing
    else:
        buf[:] = bytes(len(buf))  # fast bulk clear

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
        # round(), not int(): int() truncates toward zero, a biased rounding
        # rule that could contribute to axis-aligned density at pixel level.
        px = CENTER + round(rx3 * scale)
        py = CENTER - round(ry3 * scale)

        blend_electron(buf, px, py, *colors[i])

    # Nucleus on top -- see this function's docstring for why it must be last.
    draw_nucleus(buf)


def draw_bounding_circle(draw, r_ref, scale, outline_color=BOUNDING_SPHERE_COLOR):
    """Just the plain r_ref-radius outline circle -- the silhouette-tracking
    part of draw_orbit_marker(), split out so callers that don't want its
    rotating spoke/text marker (e.g. atom_view_pc.py's dissection view,
    which draws the bounding circle directly) can still draw a stable
    reference-sphere outline in a chosen color.
    """
    px_r = r_ref * scale
    draw.ellipse((CENTER - px_r, CENTER - px_r, CENTER + px_r, CENTER + px_r),
                 outline=outline_color)


def draw_orbit_marker(draw, r_ref, scale, angle, tilt_angle, roll_angle, marker_text=MARKER_TEXT,
                       outline_color=BOUNDING_SPHERE_COLOR):
    """Bounding-sphere + rotating marker overlay -- a pure-orthographic
    rotation cue for presets whose silhouette alone doesn't show it (see
    MARKER_TEXT's comment). Free function so both viewers can reuse it
    unmodified, each with its own marker_text (element symbol for atoms) and,
    if the caller wants the bounding circle in some color other than the
    default neutral BOUNDING_SPHERE_COLOR, outline_color.
    """
    draw_bounding_circle(draw, r_ref, scale, outline_color)

    # Reference vector (horizontal_r, y0, 0) rotated by the same yaw+tilt+roll
    # transform as every sampled point (see rotate_yaw_tilt_roll()).
    horizontal_r = r_ref * math.cos(_MARKER_ELEVATION_RAD)
    y0 = r_ref * math.sin(_MARKER_ELEVATION_RAD)
    cos_yaw = math.cos(angle)
    sin_yaw = math.sin(angle)
    cos_tilt = math.cos(tilt_angle)
    sin_tilt = math.sin(tilt_angle)
    cos_roll = math.cos(roll_angle)
    sin_roll = math.sin(roll_angle)
    rx3, ry3, rz = rotate_yaw_tilt_roll(horizontal_r, y0, 0.0,
                                        cos_yaw, sin_yaw, cos_tilt, sin_tilt, cos_roll, sin_roll)
    marker_x = CENTER + rx3 * scale
    marker_y = CENTER - ry3 * scale

    # depth_frac: 0 rotating away, 1 rotating toward -- the only depth signal
    # an orthographic projection gives. Rotation preserves vector length, so
    # |rz| never exceeds r_ref.
    depth_frac = (rz / r_ref + 1.0) / 2.0 if r_ref > 1e-6 else 0.5
    marker_color = tuple(
        int(MARKER_COLOR_BEHIND[c] + depth_frac * (MARKER_COLOR_FRONT[c] - MARKER_COLOR_BEHIND[c]))
        for c in range(3))

    # Spoke from the nucleus to the marker in the same depth-interpolated
    # color, so the whole radius reads gray-behind / lit-in-front.
    draw.line((CENTER, CENTER, marker_x, marker_y), fill=marker_color)

    draw.text((marker_x, marker_y), marker_text, fill=marker_color, font=_MARKER_FONT, anchor='mm')


def draw_scale_bar(draw, pixels_per_unit, unit_label, canvas_height=HEIGHT, max_bar_px=SCALE_BAR_MAX_PX):
    """Bottom-left physical-size reference bar, like a microscope/map scale:
    a horizontal line `length` physical units long (a "nice" round number,
    see cloud_common.pick_scale_bar_length()), labeled with that length and
    unit_label. Recomputed from the frame's live pixels_per_unit every call
    so it tracks the camera's zoom-breathing/excursion dives. pixels_per_unit
    <= 0 draws nothing (defensive).
    """
    if pixels_per_unit <= 0:
        return
    length, label = cloud_common.pick_scale_bar_length(pixels_per_unit, max_bar_px)
    bar_px = length * pixels_per_unit

    x0 = SCALE_BAR_MARGIN_X
    y = canvas_height - SCALE_BAR_MARGIN_Y
    x1 = x0 + bar_px

    # Thicker bar + ticks (SCALE_BAR_LINE_WIDTH), matching the device's own
    # bar weight -- a single-pixel line reads as too thin at this canvas size.
    draw.line((x0, y, x1, y), fill=SCALE_BAR_COLOR, width=SCALE_BAR_LINE_WIDTH)
    draw.line((x0, y - SCALE_BAR_TICK_PX, x0, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR,
              width=SCALE_BAR_LINE_WIDTH)
    draw.line((x1, y - SCALE_BAR_TICK_PX, x1, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR,
              width=SCALE_BAR_LINE_WIDTH)

    # Label at SCALE_BAR_FONT_SIZE, sitting above the tick with
    # SCALE_BAR_LABEL_GAP_PX clearance -- explicit size instead of PIL's
    # tiny, hard-to-read default font.
    draw.text((x0, y - SCALE_BAR_TICK_PX - SCALE_BAR_LABEL_GAP_PX - SCALE_BAR_FONT_SIZE),
              "%s %s" % (label, unit_label), fill=SCALE_BAR_COLOR, font=_SCALE_BAR_FONT)


def advance_rotation(app):
    """Advance yaw/tilt/roll by one normal-viewing step."""
    app.angle = (app.angle + ANGLE_STEP) % app.two_pi
    app.tilt_angle = (app.tilt_angle + TILT_ANGLE_STEP) % app.two_pi
    app.roll_angle = (app.roll_angle + ROLL_ANGLE_STEP) % app.two_pi


def fly_over(app, start_scale, end_scale, frames):
    """Short, one-shot camera move -- blocking (app.root.update() per frame
    keeps the window responsive). Takes absolute scales so it can ease
    to/from anywhere, not just back to base_scale.

    Checked at the top of every iteration: `app.aborted` (set by the
    launcher's shared Escape-to-return-to-chooser handling, see
    pc/launcher.py and orbital_view_pc.py/atom_view_pc.py's
    _request_exit()). Since the only way `aborted` can flip True mid-call is
    a bound key handler firing during the `app.root.update()` call just
    below, checking here -- immediately before the NEXT frame would render
    -- stops a long fly-over within one frame of Escape instead of running
    it to completion first. Absent on standalone (non-launcher) apps, where
    getattr()'s default keeps this a no-op.

    start_scale/end_scale are also re-scaled live against app.zoom_factor
    every frame (getattr()'s 1.0 default makes this a no-op on the hydrogen
    orbital viewer, which has no manual zoom): app.root.update() below still
    pumps the mouse-wheel/+-/zoom-button handlers even while this loop
    blocks, and those handlers update app.zoom_factor immediately, but
    start_scale/end_scale themselves were captured once by the caller --
    without this rescale a zoom press mid-flight would have no visible
    effect until the animation finished, i.e. the buttons would feel
    unresponsive. See maybe_zoom_excursion()'s docstring for how this
    composes across that function's own leg boundaries too.
    """
    z0 = getattr(app, 'zoom_factor', 1.0)
    for i in range(frames):
        if getattr(app, 'aborted', False):
            print("viewer_common: fly_over() aborted at frame %d/%d" % (i, frames))
            return
        t = i / (frames - 1) if frames > 1 else 1.0
        base = start_scale + (end_scale - start_scale) * t
        scale = base * (getattr(app, 'zoom_factor', 1.0) / z0)
        render_frame(app.buf, app.preset, app.angle, app.tilt_angle, app.roll_angle, scale)
        app._blit(scale)
        app.root.update()
        advance_rotation(app)


def maybe_zoom_excursion(app, base_scale, zoom_amplitude, outer_r_ref, inner_r_ref,
                          shell_count=1, scale_factor=1.0, ease_frames_base=None):
    """If the excursion countdown expired, dive from wherever the camera
    currently is out to the shared "outside" bound (outer_r_ref x
    ZOOM_OUTER_RADIUS_FACTOR filling the frame), in through the cloud to the
    shared "deep" bound (inner_r_ref -- the FIRST/innermost shell's own
    radius -- x ZOOM_INNER_RADIUS_FACTOR filling the frame, deeper than any
    shell's own extent), and back out to the resting breathing scale; return
    True so the caller skips its normal frame render, since the dive already
    blitted every frame of itself. `base_scale`/`zoom_amplitude` come from
    the caller so the atom viewer can dive relative to the user's manual
    zoom factor; `scale_factor` (typically that same manual zoom factor)
    scales the two bounds the same way. `shell_count` stretches every leg
    (see shell_count_frames()) so a heavier, multi-shell atom's dive isn't
    rushed. zoom_angle resets to 0 after -- sin(0) == 0 lines up exactly with
    where the dive left off. `ease_frames_base`, if given, overrides the
    shared ZOOM_EXCURSION_EASE_FRAMES_BASE for per-viewer dive pacing (the
    orbital viewer's 1.5x-slower zooms).

    `scale_factor` is only a SNAPSHOT of app.zoom_factor taken when this
    call started. fly_over() already rescales live against app.zoom_factor
    within a single leg (see its docstring), but outer_scale/inner_scale/
    base_scale below are plain numbers baked from that snapshot -- without
    re-deriving them fresh (via _live() below) right before each leg, a zoom
    press during one leg would only partially show (fly_over()'s own
    within-leg rescale) and then visibly pop back at the next leg's
    boundary, since that next leg would otherwise resume from the stale
    snapshot instead of where the previous leg actually left off on screen.
    """
    app.zoom_excursion_countdown -= 1
    if app.zoom_excursion_countdown > 0:
        return False

    def _live(value):
        return value * (getattr(app, 'zoom_factor', scale_factor) / scale_factor) if scale_factor else value

    current_scale = base_scale + zoom_amplitude * math.sin(app.zoom_angle)
    outer_scale = outer_bound_scale(outer_r_ref, scale_factor)
    inner_scale = inner_bound_scale(inner_r_ref, scale_factor)
    # ease_frames_base overrides the shared ZOOM_EXCURSION_EASE_FRAMES_BASE for
    # callers that want a different dive cadence -- e.g. orbital_view_pc.py's
    # 1.5x-slower zooms (mirroring src/views/orbital_view.cpp's local 1.5x copies of
    # camera.h's pacing constants).
    frames = shell_count_frames(ease_frames_base or ZOOM_EXCURSION_EASE_FRAMES_BASE,
                                ZOOM_EXCURSION_EASE_FRAMES_PER_SHELL, shell_count)
    fly_over(app, current_scale, _live(outer_scale), frames)
    fly_over(app, _live(outer_scale), _live(inner_scale), frames)
    fly_over(app, _live(inner_scale), _live(base_scale), frames)
    app.zoom_angle = 0.0
    app.zoom_excursion_countdown = _next_zoom_excursion_countdown()
    # If Escape landed mid-dive, one of the fly_over() calls above returned
    # early -- don't reschedule _tick(); the caller's own abort check (see
    # fly_over()'s docstring) takes it from here instead.
    if not getattr(app, 'aborted', False):
        app.root.after(FRAME_DELAY_MS, app._tick)
    return True


def blit_to_canvas(app, overlays):
    """Convert app.buf to a tkinter canvas image, letting `overlays(draw)`
    add PIL overlays (marker, scale bar, title) in between. Shared by both
    viewers' _blit methods.
    """
    image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(app.buf), 'raw', 'RGB', 0, 1)
    draw = ImageDraw.Draw(image)
    overlays(draw)
    image = image.resize(DISPLAY_SIZE, Image.NEAREST)
    app.photo = ImageTk.PhotoImage(image)
    app.canvas.itemconfig(app.image_id, image=app.photo)
