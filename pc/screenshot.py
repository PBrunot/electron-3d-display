"""pc/screenshot.py -- headless screenshot + animation generator for the PC
debug simulator (orbital_view_pc.py / atom_view_pc.py): renders the same
frames the tkinter windows show (same seeded point clouds, same camera pose,
same overlays) straight to PNG/GIF files in the repo's img/ folder, for the
GitHub README's visual illustrations.

Run:

    python3 pc/screenshot.py                      # everything below
    python3 pc/screenshot.py --orbitals           # hydrogen orbital presets + 4x4 gallery
    python3 pc/screenshot.py --atoms 6 26         # just those elements
    python3 pc/screenshot.py --dissect 20         # just that shell-dissection journey
    python3 pc/screenshot.py --orbitals-gif       # animated orbital showcase (img/orbitals.gif)
    python3 pc/screenshot.py --dissect-gif 20     # animated dissection journey (img/dissect_<sym>.gif)

No tkinter window is opened: only the shared render functions and overlay
drawing are reused (see viewer_common.blit_to_canvas() for what is skipped
-- the canvas resize/PhotoImage part). Every output is deterministic (the
samplers use fixed seeds), so rerunning the script reproduces the images.

Animated GIFs replay the ORIGINAL viewer loops -- the same per-source-frame
rotation/zoom-breathing/point-turnover as orbital_view_pc.py's _tick() for
the orbital showcase, and the same eased zoom/hold legs as atom_view_pc.py's
_run_dissection() for the dissection (the generated journey deliberately
drops the app's debug clip plane: no half is ever cut away, and the
bounding circle is plain gray, not shell-colored) -- at FULL resolution (480x480,
like the still PNGs), with the source's own per-frame cadence and frame
counts (switch fly-over lengths, dissection zoom legs). Only every
GIF_SUBSAMPLE-th source frame is written, played back at the same wall-clock
speed, so the GIFs are temporally identical to the source at a lower frame
rate. Tunables: ORBITAL_GIF_SECONDS (1s per orbital),
DISSECT_GIF_HOLD_SECONDS (1s per dissected layer), and GIF_FPS (lower =
smaller file; actual FPS = 1000 / (GIF_SUBSAMPLE * 20ms)).
"""

import argparse
import math
import os
import sys

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import atom_cloud
import cloud_common
import slater

from atom_view_pc import (
    AtomPreset, draw_atom_title, render_dissection_frame,
    DISSECT_TARGET_PX, DISSECT_CLIP_CLOSED,
    DISSECT_LABEL_COLOR, Z_NOTE_COLOR, ROLL_ANGLE_STEP,
    DISSECT_ZOOM_FRAMES, DISSECT_FRAME_DELAY_S,
)
from orbital_view_pc import Preset
from viewer_common import (
    WIDTH, HEIGHT, CENTER, PROTON_SIZE,
    TITLE_POS, SUBTITLE_POS,
    _TILT_ANGLE_START, _ROLL_ANGLE_START,
    ANGLE_STEP, TILT_ANGLE_STEP, ZOOM_ANGLE_STEP,
    FRAME_DELAY_MS, SWITCH_START_SCALE_FACTOR, SWITCH_TRANSITION_FRAMES,
    render_frame, draw_orbit_marker, draw_bounding_circle, draw_scale_bar,
)

from PIL import Image, ImageDraw, ImageFont

IMG_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'img'))

# Same initial camera pose the tkinter apps boot with (see viewer_common.py's
# _TILT_ANGLE_START/_ROLL_ANGLE_START), so every screenshot matches what the
# viewer actually shows at rest.
CAMERA = (0.0, _TILT_ANGLE_START, _ROLL_ANGLE_START)

# Curated element selection for the atom gallery: one representative per
# period/trend inside the Clementi-Raimondi-validated range (Z <= 54, see
# atom_cloud.py's calibration notes).
DEFAULT_ATOMS = (1, 2, 3, 6, 7, 8, 10, 11, 12, 13, 15, 16, 17, 18, 19, 20,
                 26, 29, 30, 35, 36, 37, 47, 54)

# Shell-dissection journeys: calcium (the classic K/L/M/N "onion") and iron
# (the 4s/3d crossover, where the outermost subshell by measured radius is
# not the one quantum numbers alone would predict).
DEFAULT_DISSECT = (20, 26)

# Per-atom screenshot zoom: each element's outer reference sphere (r_ref) is
# zoomed to this on-screen radius, matching the framing the orbital presets
# use (cloud_common.P90_TARGET_PX), so shell colors and outer-subshell lobes
# are legible in the individual atom illustrations. The atom gallery keeps
# the base scale (no zoom) so the real periodic size trend still reads there.
ATOM_ZOOM_TARGET_PX = 175.0

# Orbital screenshots use the SAME framing as the atom screenshots (their
# p90 reference radius also lands at ATOM_ZOOM_TARGET_PX), so the orbital
# and atom illustrations read at a consistent on-screen size -- the live
# viewer's own resting scale (cloud_common.P90_TARGET_PX = 100 px) would
# otherwise make every orbital visibly smaller than every atom. Kept as an
# alias of ATOM_ZOOM_TARGET_PX so tuning one retunes both.
ORBITAL_ZOOM_TARGET_PX = ATOM_ZOOM_TARGET_PX

# --- Animated GIFs ------------------------------------------------------------
# GIFs replay the SOURCE animations: same per-frame physics (rotation steps,
# zoom breathing, point-turnover cadence, dissection ease legs, switch
# fly-over lengths) and the same wall-clock duration, at FULL resolution
# (480x480 -- GIF_SIZE = 1.0, no downscaling). Full-res frames at the
# source's 50fps would be ~33 KB/frame, so instead the GIF writes every
# GIF_SUBSAMPLE-th source frame and plays them back at the same speed: a
# lower-FPS but temporally identical replay.
#
# Tunables: ORBITAL_GIF_SECONDS (spin per orbital), DISSECT_GIF_HOLD_SECONDS
# (hold per dissected layer), and GIF_FPS (nominal GIF frame rate -- the
# subsample factor derives from it as round(50/GIF_FPS), so actual FPS =
# 1000 / (GIF_SUBSAMPLE * 20ms)). Lower GIF_FPS = smaller file.
GIF_SIZE = 1.0
GIF_FPS = 12                                 # nominal GIF frame rate (12.5 actual)
GIF_SUBSAMPLE = max(1, round(50.0 / GIF_FPS))  # write every Nth source frame
GIF_FRAME_MS = GIF_SUBSAMPLE * FRAME_DELAY_MS  # playback delay of written frames
ORBITAL_GIF_SECONDS = 1.5                    # spin time per orbital
DISSECT_GIF_HOLD_SECONDS = 1.5               # hold time per dissected layer
DEFAULT_DISSECT_GIF_Z = 26


def fresh_buf():
    """A zeroed RGB byte buffer, exactly like the viewers' app.buf."""
    return bytearray(WIDTH * HEIGHT * 3)


def render_to_image(buf, overlays):
    """Turn buf + PIL overlays into a WIDTHxHEIGHT RGB image. Mirrors
    blit_to_canvas() minus the tkinter resize/PhotoImage part.
    """
    image = Image.frombuffer('RGB', (WIDTH, HEIGHT), bytes(buf), 'raw', 'RGB', 0, 1)
    draw = ImageDraw.Draw(image)
    overlays(draw)
    return image


def save_frame(buf, overlays, relpath):
    """Write buf + overlays to IMG_DIR/relpath as a PNG."""
    image = render_to_image(buf, overlays)
    path = os.path.join(IMG_DIR, relpath)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.save(path)
    print('saved %s' % relpath)


def montage(cells, relpath, caption_h=56, font_size=26, columns=None):
    """A black-background strip/grid of (image-or-path, caption) cells at
    FULL source resolution (no downscaling -- each cell is the 480x480
    screenshot itself), with the caption underneath. columns=None lays all
    cells out in one horizontal row.
    """
    if not cells:
        return
    loaded = []
    for item in cells:
        if isinstance(item[0], str):
            loaded.append((Image.open(os.path.join(IMG_DIR, item[0])).convert('RGB'), item[1]))
        else:
            loaded.append((item[0], item[1]))
    cell_px = loaded[0][0].size[0]
    cols = columns if columns is not None else len(loaded)
    rows = (len(loaded) + cols - 1) // cols
    canvas = Image.new('RGB', (cols * cell_px, rows * (cell_px + caption_h)), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default(size=font_size)
    for i, (img, cap) in enumerate(loaded):
        r, c = divmod(i, cols)
        x, y = c * cell_px, r * (cell_px + caption_h)
        canvas.paste(img, (x, y))
        draw.text((x + 6, y + cell_px + 8), cap, fill=(225, 225, 225), font=font)
    canvas.save(os.path.join(IMG_DIR, relpath))
    print('saved %s' % relpath)


# --- GIF plumbing -------------------------------------------------------------

def _shared_palette(frames, sample=12, colors=256):
    """One 256-color palette covering every frame's colors: MEDIANCUT over a
    handful of representative frames concatenated side by side, then reused
    to map every frame -- cheaper and more color-consistent than per-frame
    clustering, and lets the GIF carry a single global palette.
    """
    step = max(1, len(frames) // sample)
    picked = frames[::step][:sample]
    w, h = picked[0].size
    strip = Image.new('RGB', (w * len(picked), h))
    for i, f in enumerate(picked):
        strip.paste(f, (i * w, 0))
    return strip.quantize(colors=colors, method=Image.Quantize.MEDIANCUT,
                          dither=Image.Dither.NONE)


def save_gif(frames, relpath, duration_ms=GIF_FRAME_MS):
    """Quantize RGB frames to one shared palette and write a looping GIF."""
    pal = _shared_palette(frames)
    mapped = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    path = os.path.join(IMG_DIR, relpath)
    mapped[0].save(path, save_all=True, append_images=mapped[1:],
                   duration=duration_ms, loop=0, disposal=2)
    print('saved %s (%d frames, %.1f MB, %ds at %dms)' % (
        relpath, len(frames), os.path.getsize(path) / (1024 * 1024),
        len(frames) * duration_ms // 1000, duration_ms))


def _advance(app_state):
    """Advance yaw/tilt/roll/zoom by one frame, exactly like
    viewer_common.advance_rotation() plus the zoom-breathing phase."""
    state = app_state
    state['angle'] = (state['angle'] + ANGLE_STEP) % state['two_pi']
    state['tilt'] = (state['tilt'] + TILT_ANGLE_STEP) % state['two_pi']
    state['roll'] = (state['roll'] + ROLL_ANGLE_STEP) % state['two_pi']
    state['zoom'] = (state['zoom'] + ZOOM_ANGLE_STEP) % state['two_pi']


# --- Hydrogen orbitals ---------------------------------------------------------

def _orbital_overlays(preset, scale, angle, tilt, roll):
    def overlays(draw):
        draw_orbit_marker(draw, preset.r_ref, scale, angle, tilt, roll)
        draw_scale_bar(draw, scale / cloud_common.PM_PER_BOHR, "pm")
        draw.text(TITLE_POS, preset.title, fill=(255, 255, 255))
    return overlays


def orbital_screenshots(zoom_target=ORBITAL_ZOOM_TARGET_PX):
    """One PNG per ORBITAL_PRESETS entry, zoomed so the preset's p90
    reference radius (r_ref) lands at zoom_target pixels -- the same framing
    the atom screenshots use, so orbitals and atoms read at a consistent
    size. Same zoom math as atom_screenshots(); overlays are the live
    viewer's bounding-sphere marker, scale bar, and title.
    """
    angle, tilt, roll = CAMERA
    for index in range(len(cloud_common.ORBITAL_PRESETS)):
        label = cloud_common.ORBITAL_PRESETS[index][3]
        preset = Preset(index)
        buf = fresh_buf()
        scale = preset.base_scale * (zoom_target / max(preset.r_ref * preset.base_scale, 1e-6))
        render_frame(buf, preset, angle, tilt, roll, scale, buzz_fraction=0.0)
        save_frame(buf, _orbital_overlays(preset, scale, angle, tilt, roll), 'orbital_%s.png' % label)


def orbital_gallery():
    cells = [('orbital_%s.png' % cloud_common.ORBITAL_PRESETS[i][3],
              cloud_common.ORBITAL_PRESETS[i][3]) for i in range(len(cloud_common.ORBITAL_PRESETS))]
    montage(cells, 'orbital_gallery.png', columns=4)


def orbital_animation_gif():
    """The original orbital-viewer loop, one preset at a time: each of the
    16 ORBITAL_PRESETS spins for ORBITAL_GIF_SECONDS (default 1s) with the
    same per-source-frame rotation + zoom breathing + point-turnover cadence
    as the live app (buzz disabled, as on PC -- see DEBUG_DISABLE_BUZZ),
    then the next preset flies in from far away over SWITCH_TRANSITION_FRAMES
    (18) source frames exactly like a nudge switch. The simulation advances
    at the source's own 20ms cadence; only every GIF_SUBSAMPLE-th source
    frame is written to the GIF, played back at GIF_FRAME_MS -- same motion,
    same duration, lower FPS. Output: img/orbitals.gif.
    """
    presets = [Preset(i) for i in range(len(cloud_common.ORBITAL_PRESETS))]
    state = {'angle': CAMERA[0], 'tilt': CAMERA[1], 'roll': CAMERA[2],
             'zoom': 0.0, 'two_pi': 2 * math.pi}
    frames = []
    n_presets = len(presets)
    frames_per = int(round(ORBITAL_GIF_SECONDS * 1000.0 / FRAME_DELAY_MS))
    transition_frames = SWITCH_TRANSITION_FRAMES
    sub = GIF_SUBSAMPLE

    def snap(preset, scale, buf):
        render_frame(buf, preset, state['angle'], state['tilt'], state['roll'], scale,
                     buzz_fraction=0.0)
        frames.append(render_to_image(
            buf, _orbital_overlays(preset, scale, state['angle'], state['tilt'], state['roll'])))

    for index in range(n_presets):
        preset = presets[index]
        buf = fresh_buf()
        cull_count = max(1, int(len(preset.xs) * cloud_common.CULL_FRACTION))
        cull_frame_count = 0
        for src_i in range(frames_per):
            if cull_frame_count >= cloud_common.CULL_REFRESH_FRAMES:
                preset.resample(cull_count)
                cull_frame_count = 0
            scale = preset.base_scale + preset.zoom_amplitude * math.sin(state['zoom'])
            if src_i % sub == 0:
                snap(preset, scale, buf)
            _advance(state)
            cull_frame_count += 1

        # Fly the NEXT preset in from far away -- the app's nudge switch
        # (SWITCH_START_SCALE_FACTOR x base -> base).
        if index < n_presets - 1:
            nxt = presets[index + 1]
            buf = fresh_buf()
            start_scale = nxt.base_scale * SWITCH_START_SCALE_FACTOR
            for src_i in range(transition_frames):
                t = src_i / (transition_frames - 1) if transition_frames > 1 else 1.0
                scale = start_scale + (nxt.base_scale - start_scale) * t
                if src_i % sub == 0:
                    snap(nxt, scale, buf)
                _advance(state)

    save_gif(frames, 'orbitals.gif')


# --- Multi-electron atoms ------------------------------------------------------

def atom_screenshots(z_list, zoom_target=ATOM_ZOOM_TARGET_PX):
    """One PNG per element, zoomed so the element's outer reference sphere
    (r_ref) lands at zoom_target pixels -- the same framing the orbital
    presets use, making shell colors and outer-subshell lobes legible. The
    zoom is exactly the viewer's manual zoom: scale = base_scale * factor,
    applied to the point render and every overlay alike. Overlays match the
    atom viewer's normal (non-dissecting) HUD: element marker, gray bounding
    circle, scale bar, shell-colored config title.
    """
    angle, tilt, roll = CAMERA
    for z in z_list:
        sym = slater.element_symbol(z)
        preset = AtomPreset(z)
        buf = fresh_buf()
        scale = preset.base_scale * (zoom_target / max(preset.r_ref * preset.base_scale, 1e-6))
        render_frame(buf, preset, angle, tilt, roll, scale)

        def overlays(draw):
            draw_orbit_marker(draw, preset.r_ref, scale, angle, tilt, roll, marker_text=sym)
            draw_scale_bar(draw, scale / atom_cloud.PM_PER_BOHR, "pm")
            draw_atom_title(draw, TITLE_POS[0], TITLE_POS[1], z, preset.config)

        save_frame(buf, overlays, 'atom_%s.png' % sym)


# (period, group) of every element Z=1..54 in the standard 18-column
# periodic table. Covers the full Clementi-Raimondi-validated range (H..Xe):
# periods 1-5 completely, no f-block (La=57 is past the cutoff). Z>54 is
# excluded on purpose -- slater.z_eff_radial()'s Slater fallback past the CR
# table produces a known size discontinuity (see atom_cloud.py's
# _CALIBRATION_Z comment), which would break the honest size comparison this
# gallery exists for.
PERIODIC_TABLE = {
    1: (1, 1), 2: (1, 18),
    3: (2, 1), 4: (2, 2), 5: (2, 13), 6: (2, 14), 7: (2, 15), 8: (2, 16),
    9: (2, 17), 10: (2, 18),
    11: (3, 1), 12: (3, 2), 13: (3, 13), 14: (3, 14), 15: (3, 15), 16: (3, 16),
    17: (3, 17), 18: (3, 18),
    19: (4, 1), 20: (4, 2), 21: (4, 3), 22: (4, 4), 23: (4, 5), 24: (4, 6),
    25: (4, 7), 26: (4, 8), 27: (4, 9), 28: (4, 10), 29: (4, 11), 30: (4, 12),
    31: (4, 13), 32: (4, 14), 33: (4, 15), 34: (4, 16), 35: (4, 17), 36: (4, 18),
    37: (5, 1), 38: (5, 2), 39: (5, 3), 40: (5, 4), 41: (5, 5), 42: (5, 6),
    43: (5, 7), 44: (5, 8), 45: (5, 9), 46: (5, 10), 47: (5, 11), 48: (5, 12),
    49: (5, 13), 50: (5, 14), 51: (5, 15), 52: (5, 16), 53: (5, 17), 54: (5, 18),
}
PERIODIC_COLS = 18
PERIODIC_ROWS = 5


def atom_gallery():
    """The atom gallery arranged like a periodic table: every element from
    H (Z=1) to Xe (Z=54) -- the Clementi-Raimondi-validated range -- placed
    at its (period, group) cell, ALL rendered at the same physical scale
    (PIXELS_PER_BOHR, the viewer's resting base_scale -- no per-element
    zoom), so the real size trend across periods and groups reads directly
    (tight noble-gas cores vs diffuse alkali atoms). Cells are the full
    480x480 source frames (no downscaling); the periodic-table shape comes
    from the empty transition-metal cells in periods 2-3. Each cell draws
    the cloud + nucleus + its plain gray bounding circle + the physical
    scale bar (identical in every cell -- same pixels-per-Bohr), the atomic
    number with its electron configuration (orbital composition) right
    after it in the cell's top-left corner (classic element-box style, the
    config font sized so even Xe's full config fits), the symbol in the
    caption below, and a 1-px frame around the cell.
    """
    angle, tilt, roll = CAMERA
    cell_px = 480
    caption_h = 56
    header_h = 46
    W = PERIODIC_COLS * cell_px
    H = header_h + PERIODIC_ROWS * (cell_px + caption_h)
    canvas = Image.new('RGB', (W, H), (0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    gfont = ImageFont.load_default(size=24)
    for g in range(1, PERIODIC_COLS + 1):
        draw.text(((g - 1) * cell_px + cell_px // 2, 8), str(g),
                  fill=(150, 150, 150), font=gfont, anchor='ma')
    cfont = ImageFont.load_default(size=26)
    zfont = ImageFont.load_default(size=32)
    cfg_font = ImageFont.load_default(size=18)
    for z in range(1, 55):
        period, group = PERIODIC_TABLE[z]
        preset = AtomPreset(z)
        buf = fresh_buf()
        scale = preset.base_scale * 3
        def overlays(draw):
            draw_bounding_circle(draw, preset.r_ref, scale)
            # The same physical scale in every cell: an identical bar in all
            # 54 cells is the visual proof of the uniform pixels-per-Bohr.
            draw_scale_bar(draw, scale / atom_cloud.PM_PER_BOHR, "pm")

        render_frame(buf, preset, angle, tilt, roll, scale)
        img = render_to_image(buf, overlays)
        x = (group - 1) * cell_px
        y = header_h + (period - 1) * (cell_px + caption_h)
        canvas.paste(img, (x, y))
        # Atomic number in the cell's top-left corner, with the electron
        # configuration (orbital composition) right after it, like a classic
        # periodic-table element box; the caption below keeps just the symbol.
        z_width = draw.textlength(str(z), font=zfont)
        draw.text((x + 10, y + 8), str(z), fill=(225, 225, 225), font=zfont)
        draw.text((x + 10 + z_width + 12, y + 16), slater.configuration_str(preset.config),
                  fill=(200, 200, 200), font=cfg_font)
        draw.text((x + 6, y + cell_px + 8), slater.element_symbol(z),
                  fill=(225, 225, 225), font=cfont)
        # 1-px frame around the cell.
        draw.rectangle((x, y, x + cell_px - 1, y + cell_px - 1), outline=(90, 90, 90))
    canvas.save(os.path.join(IMG_DIR, 'atom_gallery.png'))
    print('saved atom_gallery.png (periodic table, Z=1..54, uniform scale)')


# --- Shell-dissection journey --------------------------------------------------

def _dissect_overlays(preset, z, scale, r_ref, label):
    """The dissection HUD: plain gray bounding circle (neutral
    BOUNDING_SPHERE_COLOR, not shell-colored), scale bar, config title,
    optional yellow subshell label, and the red Z note by the nucleus.
    """

    def overlays(draw):
        draw_bounding_circle(draw, r_ref, scale)
        draw_scale_bar(draw, scale / atom_cloud.PM_PER_BOHR, "pm")
        draw_atom_title(draw, TITLE_POS[0], TITLE_POS[1], z, preset.config)
        if label:
            draw.text(SUBTITLE_POS, label, fill=DISSECT_LABEL_COLOR)
        draw.text((CENTER + PROTON_SIZE, CENTER - PROTON_SIZE), "Z=%d" % z, fill=Z_NOTE_COLOR)

    return overlays


def _dissect_label(subshell_str, n, ell, letter, ecount):
    return '%s: n=%d, l=%d (%s shell) -- %d electron%s' % (
        subshell_str, n, ell, letter, ecount, '' if ecount == 1 else 's')


def dissection_screenshots(z):
    """The dissection journey for one element, as still frames: complete
    cloud -> each subshell zoomed in and highlighted, outermost to
    innermost -> zoom back out. No clipping anywhere: unlike the app's
    debug dissection, the full cloud stays visible in every frame (and the
    bounding circle is plain gray, not shell-colored). Returns the (relpath,
    caption) cells in journey order for the montage.
    """
    sym = slater.element_symbol(z)
    preset = AtomPreset(z)
    plan = atom_cloud.subshell_dissection_plan(
        preset.xs, preset.ys, preset.zs, preset.shells, preset.ells, preset.config)

    angle, tilt, roll = CAMERA
    buf = fresh_buf()
    base_scale = preset.base_scale
    cells = []

    # Phase 0: complete cloud, resting scale.
    render_dissection_frame(buf, preset, angle, tilt, roll, base_scale, DISSECT_CLIP_CLOSED, None)
    relpath = 'dissect_%s_0_full.png' % sym
    save_frame(buf, _dissect_overlays(preset, z, base_scale, preset.r_ref, None), relpath)
    cells.append((relpath, 'complete cloud'))

    # Phases 1..N: each subshell zoomed to fill the frame, highlighted in
    # phase colors, everything else dimmed gray, label shown -- outermost
    # (largest measured radius) to innermost, same order as the app. The
    # full cloud stays visible: no clip plane.
    for k, (n, ell, letter, subshell_str, ecount, r_ref) in enumerate(plan, start=1):
        target_scale = DISSECT_TARGET_PX / max(r_ref, 1e-6)
        label = _dissect_label(subshell_str, n, ell, letter, ecount)
        render_dissection_frame(buf, preset, angle, tilt, roll, target_scale, DISSECT_CLIP_CLOSED, (n, ell))
        relpath = 'dissect_%s_%d_%s.png' % (sym, k, subshell_str)
        save_frame(buf, _dissect_overlays(preset, z, target_scale, r_ref, label), relpath)
        cells.append((relpath, '%s (%s shell)' % (subshell_str, letter)))

    # Phase N+1: zoom back out to the resting scale, full cloud again.
    render_dissection_frame(buf, preset, angle, tilt, roll, base_scale, DISSECT_CLIP_CLOSED, None)
    relpath = 'dissect_%s_%d_back.png' % (sym, len(plan) + 1)
    save_frame(buf, _dissect_overlays(preset, z, base_scale, preset.r_ref, None), relpath)
    cells.append((relpath, 'zoom back out'))

    return cells


def journey_montage(z, cells):
    montage(cells, 'dissect_%s_journey.png' % slater.element_symbol(z), columns=len(cells))


def hero_montage(dissect_cells):
    """One hero strip for the README top: a 1s sphere, a 2p_z lobe pair, a
    carbon atom, and the first step of a dissection journey.
    """
    cells = [
        ('orbital_1s.png', '1s'),
        ('orbital_2p_z.png', '2p_z'),
        ('atom_C.png', 'C (Z=6)'),
        (dissect_cells[0][0], dissect_cells[0][1]),
    ]
    montage(cells, 'hero.png', columns=4)


def dissection_animation_gif(z=DEFAULT_DISSECT_GIF_Z):
    """The dissection journey, animated: each subshell zooms in (over the
    source's DISSECT_ZOOM_FRAMES), is highlighted in phase colors with the
    others dimmed gray, and holds with its label for DISSECT_GIF_HOLD_SECONDS
    (default 1s per layer), outermost to innermost; then it zooms back out to
    the whole atom. Deliberately no clip plane -- unlike the app's debug
    dissection (atom_view_pc.py's _run_dissection()), the journey never cuts
    the cloud or hides a half; the full cloud stays visible in every frame.
    Only roll tumbles throughout, paced at the source's own
    DISSECT_FRAME_DELAY_S. Output: img/dissect_<sym>.gif.
    """
    sym = slater.element_symbol(z)
    preset = AtomPreset(z)
    plan = atom_cloud.subshell_dissection_plan(
        preset.xs, preset.ys, preset.zs, preset.shells, preset.ells, preset.config)

    angle, tilt, roll = CAMERA
    two_pi = 2 * math.pi
    buf = fresh_buf()
    base_scale = preset.base_scale
    frames = []
    sub = GIF_SUBSAMPLE

    def snap(scale, clip, active, r_ref, label):
        render_dissection_frame(buf, preset, angle, tilt, roll, scale, clip, active)
        frames.append(render_to_image(buf, _dissect_overlays(preset, z, scale, r_ref, label)))

    def tumble():
        nonlocal roll
        roll = (roll + ROLL_ANGLE_STEP) % two_pi

    def ease(scale0, scale1, clip0, clip1, active, r_ref, n, label=None):
        nonlocal angle, tilt, roll
        for i in range(n):
            t = i / (n - 1) if n > 1 else 1.0
            if i % sub == 0:
                snap(scale0 + (scale1 - scale0) * t, clip0 + (clip1 - clip0) * t,
                     active, r_ref, label)
            tumble()

    def hold(scale, clip, active, r_ref, n, label):
        for i in range(n):
            if i % sub == 0:
                snap(scale, clip, active, r_ref, label)
            tumble()

    # The journey: zoom into each subshell (outermost to innermost),
    # highlight it while dimming the rest, hold with its label, then zoom
    # back out to the whole atom. No clip plane anywhere -- the full cloud
    # stays visible in every frame. The zoom legs use the source's
    # DISSECT_ZOOM_FRAMES; only the hold length is tunable
    # (DISSECT_GIF_HOLD_SECONDS).
    hold_frames = int(round(DISSECT_GIF_HOLD_SECONDS / DISSECT_FRAME_DELAY_S))
    prev_scale = base_scale
    for n, ell, letter, subshell_str, ecount, r_ref in plan:
        target_scale = DISSECT_TARGET_PX / max(r_ref, 1e-6)
        label = _dissect_label(subshell_str, n, ell, letter, ecount)
        ease(prev_scale, target_scale, DISSECT_CLIP_CLOSED, DISSECT_CLIP_CLOSED, (n, ell),
             r_ref, DISSECT_ZOOM_FRAMES, label)
        hold(target_scale, DISSECT_CLIP_CLOSED, (n, ell), r_ref,
             hold_frames, label)
        prev_scale = target_scale

    # Zoom back out to the resting scale, full cloud again.
    ease(prev_scale, base_scale, DISSECT_CLIP_CLOSED, DISSECT_CLIP_CLOSED, None,
         preset.r_ref, DISSECT_ZOOM_FRAMES)

    save_gif(frames, 'dissect_%s.gif' % sym)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--orbitals', action='store_true',
                        help='hydrogen orbital presets (PNGs + 4x4 gallery)')
    parser.add_argument('--atoms', nargs='*', type=int, default=None, metavar='Z',
                        help='multi-electron atom screenshots for these Z values')
    parser.add_argument('--dissect', nargs='*', type=int, default=None, metavar='Z',
                        help='shell-dissection journey screenshots for these Z values')
    parser.add_argument('--orbitals-gif', action='store_true',
                        help='animated orbital showcase (img/orbitals.gif)')
    parser.add_argument('--dissect-gif', nargs='?', type=int, const=DEFAULT_DISSECT_GIF_Z,
                        default=None, metavar='Z',
                        help='animated dissection journey for this Z (default %d)' % DEFAULT_DISSECT_GIF_Z)
    args = parser.parse_args()

    everything = not (args.orbitals or args.atoms is not None or args.dissect is not None
                      or args.orbitals_gif or args.dissect_gif is not None)

    if everything or args.orbitals:
        orbital_screenshots()
        orbital_gallery()

    z_atoms = args.atoms if args.atoms is not None else DEFAULT_ATOMS
    if everything or args.atoms is not None:
        atom_screenshots(z_atoms)
        atom_gallery()  # periodic-table gallery, always the full H..Xe set

    z_dissect = args.dissect if args.dissect is not None else DEFAULT_DISSECT
    if everything or args.dissect is not None:
        journey_cells = None
        for z in z_dissect:
            cells = dissection_screenshots(z)
            journey_montage(z, cells)
            journey_cells = cells
        if journey_cells:
            hero_montage(journey_cells)

    if everything or args.orbitals_gif:
        orbital_animation_gif()

    if everything or args.dissect_gif is not None:
        dissection_animation_gif(args.dissect_gif if args.dissect_gif is not None else DEFAULT_DISSECT_GIF_Z)


if __name__ == '__main__':
    main()
