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

# --- Display geometry -------------------------------------------------------
WIDTH = 480
HEIGHT = 480
CENTER = WIDTH // 2
DISPLAY_SCALE = 2  # tkinter window is WIDTH*DISPLAY_SCALE square; math stays at WIDTH/HEIGHT
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
FRAME_DELAY_MS = 20  # tkinter .after() delay; PC is fast enough this is the real throttle

# --- Intro / preset-switch transitions --------------------------------------
INTRO_START_SCALE_FACTOR = 12.0
INTRO_FRAMES = 70
SWITCH_START_SCALE_FACTOR = 10.0
SWITCH_TRANSITION_FRAMES = 18

# --- Random zoom excursions -------------------------------------------------
# At randomized intervals, dive from the current breathing scale to a
# randomized target and back, layered on the constant sine breathing so the
# motion doesn't read as purely mechanical. Max factor is deeper than the
# device's 5.0 -- no render budget to protect, so a dive can feel like
# passing through the cloud itself.
ZOOM_EXCURSION_MIN_INTERVAL_FRAMES = 150
ZOOM_EXCURSION_MAX_INTERVAL_FRAMES = 400
ZOOM_EXCURSION_SCALE_MIN_FACTOR = 0.35
ZOOM_EXCURSION_SCALE_MAX_FACTOR = 9.0
ZOOM_EXCURSION_EASE_FRAMES = 30

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
PROTON_SIZE = 3
PROTON_COLOR = (255, 0, 0)

# --- Electron point rendering ------------------------------------------------
# Each point alpha-blends toward its own color instead of overwriting the
# pixel (1.0 = opaque). Overlapping points converge toward full brightness,
# so apparent brightness tracks local sample DENSITY at a pixel -- the way a
# translucent point cloud reads. The nucleus above is NOT blended (one literal
# particle, not a probability cloud).
ELECTRON_ALPHA = 0.5

# Phosphor-style persistence (PC-only cosmetic; the device hard-clears each
# frame -- see orbital_view.py). Each frame fades the previous buffer toward
# black via bytes.translate() (one C-level pass, effectively free at
# 240x240x3 bytes/frame) instead of clearing, so points leave a trailing glow
# and skipped "buzz" points fade out instead of vanishing.
ENABLE_PERSISTENCE = True
PERSISTENCE_DECAY = 100  # /256 kept per frame (~0.39); lower = shorter trails, 256 = never fades
_PERSISTENCE_TABLE = bytes((i * PERSISTENCE_DECAY) // 256 for i in range(256))

# --- Scale bar (bottom-left physical-size reference) ------------------------
# "Nice" round lengths + the picking rule live in cloud_common.py
# (pick_scale_bar_length()), shared with the device renderer so a bar reads
# the same physical length on both. What's left here is PIL-specific geometry.
SCALE_BAR_MARGIN_X = 8
SCALE_BAR_MARGIN_Y = 8
SCALE_BAR_MAX_PX = 90
SCALE_BAR_TICK_PX = 4
SCALE_BAR_COLOR = (210, 210, 210)

# --- HUD text positions -------------------------------------------------------
TITLE_POS = (2, 2)
SUBTITLE_POS = (2, 12)


def _next_zoom_excursion_countdown():
    return random.randint(ZOOM_EXCURSION_MIN_INTERVAL_FRAMES, ZOOM_EXCURSION_MAX_INTERVAL_FRAMES)


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


def render_frame(buf, preset, angle, tilt_angle, roll_angle, scale, buzz_fraction=0.0):
    """Clear (or fade, see ENABLE_PERSISTENCE) `buf`, draw the nucleus, then
    rotate (yaw/tilt/roll -- all three needed, see orbital_view.py's module
    docstring) and alpha-blend every point of `preset` into the buffer.
    `preset` need only expose xs/ys/zs/colors (Preset and AtomPreset both do).
    """
    if ENABLE_PERSISTENCE:
        buf[:] = buf.translate(_PERSISTENCE_TABLE)  # fade previous frame instead of clearing
    else:
        buf[:] = bytes(len(buf))  # fast bulk clear

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
        # round(), not int(): int() truncates toward zero, a biased rounding
        # rule that could contribute to axis-aligned density at pixel level.
        px = CENTER + round(rx3 * scale)
        py = CENTER - round(ry3 * scale)

        if 0 <= px < WIDTH and 0 <= py < HEIGHT:
            idx = (py * WIDTH + px) * 3
            cr, cg, cb = colors[i]
            buf[idx] = buf[idx] + int((cr - buf[idx]) * ELECTRON_ALPHA)
            buf[idx + 1] = buf[idx + 1] + int((cg - buf[idx + 1]) * ELECTRON_ALPHA)
            buf[idx + 2] = buf[idx + 2] + int((cb - buf[idx + 2]) * ELECTRON_ALPHA)


def draw_bounding_circle(draw, r_ref, scale, outline_color=BOUNDING_SPHERE_COLOR):
    """Just the plain r_ref-radius outline circle -- the silhouette-tracking
    part of draw_orbit_marker(), split out so callers that don't want its
    rotating spoke/text marker (e.g. atom_view_pc.py's dissection view,
    which already has its own reference equator ring giving a rotation cue)
    can still draw a stable reference-sphere outline in a chosen color.
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
    if the caller wants the bounding circle to match some other reference
    color (e.g. atom_view_pc.py's shell-matched equator) instead of the
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

    draw.line((x0, y, x1, y), fill=SCALE_BAR_COLOR)
    draw.line((x0, y - SCALE_BAR_TICK_PX, x0, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR)
    draw.line((x1, y - SCALE_BAR_TICK_PX, x1, y + SCALE_BAR_TICK_PX), fill=SCALE_BAR_COLOR)

    draw.text((x0, y - SCALE_BAR_TICK_PX - 12), "%s %s" % (label, unit_label), fill=SCALE_BAR_COLOR)


def advance_rotation(app):
    """Advance yaw/tilt/roll by one normal-viewing step."""
    app.angle = (app.angle + ANGLE_STEP) % app.two_pi
    app.tilt_angle = (app.tilt_angle + TILT_ANGLE_STEP) % app.two_pi
    app.roll_angle = (app.roll_angle + ROLL_ANGLE_STEP) % app.two_pi


def fly_over(app, start_scale, end_scale, frames):
    """Short, one-shot camera move -- blocking (app.root.update() per frame
    keeps the window responsive). Takes absolute scales so it can ease
    to/from anywhere, not just back to base_scale.
    """
    for i in range(frames):
        t = i / (frames - 1) if frames > 1 else 1.0
        scale = start_scale + (end_scale - start_scale) * t
        render_frame(app.buf, app.preset, app.angle, app.tilt_angle, app.roll_angle, scale)
        app._blit(scale)
        app.root.update()
        advance_rotation(app)


def maybe_zoom_excursion(app, base_scale, zoom_amplitude):
    """If the excursion countdown expired, dive to a random scale and back
    and return True -- the caller should skip its normal frame, since the
    dive already blitted every frame of itself. `base_scale`/`zoom_amplitude`
    come from the caller so the atom viewer can dive relative to the user's
    manual zoom factor. zoom_angle resets to 0 after -- sin(0) == 0 lines up
    exactly with where the dive left off.
    """
    app.zoom_excursion_countdown -= 1
    if app.zoom_excursion_countdown > 0:
        return False
    current_scale = base_scale + zoom_amplitude * math.sin(app.zoom_angle)
    target_scale = base_scale * random.uniform(ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                ZOOM_EXCURSION_SCALE_MAX_FACTOR)
    fly_over(app, current_scale, target_scale, ZOOM_EXCURSION_EASE_FRAMES)
    fly_over(app, target_scale, base_scale, ZOOM_EXCURSION_EASE_FRAMES)
    app.zoom_angle = 0.0
    app.zoom_excursion_countdown = _next_zoom_excursion_countdown()
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
