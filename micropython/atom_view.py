"""Multi-electron atom point-cloud animation for the ESP32-S3 panel -- the
device counterpart of pc/atom_view_pc.py, built the same way orbital_view.py
is: atom_cloud.py supplies the model (multi-electron point cloud, shared
unmodified with the PC viewer), device_render_common.py supplies the Q8
fixed-point/viper rendering, framebuf/ST7789 blitting, fly-over/zoom-
excursion camera, and nudge/IMU plumbing (all shared with orbital_view.py
too). What's left here is genuinely atom-specific: AtomPresetState
(shell-by-n point coloring, no point-turnover -- atom_cloud's cloud is
static, see its module docstring), _draw_atom_title() (framebuf counterpart
of pc/atom_view_pc.py's draw_atom_title() -- same per-shell-colored
electron-configuration label, wrapped across lines instead of PC's one wide
line, see that function's docstring), N_POINTS (smaller than the PC
viewer's 10000, same device rendering budget as orbital_view.py's 3000),
and a run() loop that lets nudge step the atomic number Z instead of
cycling a fixed preset list.

Not the default boot animation (CLAUDE.md's M1-M4 roadmap is single-electron
hydrogen orbitals) -- run this the same way corner_test.py is run, as a
standalone alternate entry point instead of editing main.py. After copying
micropython/. to the board (see main.py's docstring for the mpremote
incantation that actually works on this board):
    mpremote connect <port> exec "import atom_view; atom_view.run()"

No shell-dissection view here (pc/atom_view_pc.py's D-key sequence) -- that
is a PIL/tkinter-heavy PC debug feature with no framebuf equivalent worth
the effort, per this project's "dev/debug text, not worth the effort"
precedent for device overlays (see orbital_view.py's module docstring).
"""

import math
import random
import time

import array
import framebuf

import atom_cloud
import device_render_common as drc
import display as display_mod
import slater

WIDTH = drc.WIDTH
HEIGHT = drc.HEIGHT
CENTER = drc.CENTER

N_POINTS = 3000  # same device render budget as orbital_view.py's cloud_common.N_POINTS
DEFAULT_Z = 6  # carbon -- simplest element with an interesting (non-full, non-empty) p subshell

# Calibrated once for THIS panel's own CENTER (see
# atom_cloud.pixels_per_bohr_for_canvas()'s docstring for why it's a
# fraction of CENTER rather than a fixed pixel count -- the same call in
# pc/atom_view_pc.py uses the PC debug window's much larger CENTER and lands
# on a different, PC-appropriate PIXELS_PER_BOHR).
PIXELS_PER_BOHR = atom_cloud.pixels_per_bohr_for_canvas(CENTER)

FPS_UPDATE_INTERVAL = 50
FPS_TEXT_POS = (2, 2)

# framebuf's built-in font is a fixed 8x8 glyph -- no textlength() equivalent
# needed (unlike pc/atom_view_pc.py's draw_atom_title(), which measures each
# PIL-rendered segment): every character is exactly this many pixels wide.
FONT_WIDTH_PX = 8
FONT_LINE_HEIGHT_PX = 10


def _draw_atom_title(fb, x, y, z, config, text_color):
    """Device (framebuf) counterpart of pc/atom_view_pc.py's
    draw_atom_title(): element symbol + Z in `text_color`, then each
    subshell of its electron configuration ('1s2 2s2 2p2 ...') colored by
    shell (atom_cloud.SHELL_RGB[n] -- the same colors the point cloud
    itself uses), so the on-screen label and the rendered cloud read as one
    color language on both PC and device.

    Wraps to a new line (see FONT_LINE_HEIGHT_PX) instead of running off the
    right edge when a segment would cross `x + WIDTH` -- this panel is
    240px, a third of pc/atom_view_pc.py's window, so heavier elements'
    configurations (e.g. iron's "1s2 2s2 2p6 3s2 3p6 3d6 4s2", ~38 chars)
    routinely need it. `x > cursor_x`'s check is `cursor_x > x`, i.e. never
    wraps mid-segment on the first segment of a line -- an over-wide single
    segment still gets drawn (and clipped by framebuf itself) rather than
    wrapping forever.
    """
    cursor_x = x
    cursor_y = y

    def draw_segment(segment, color):
        nonlocal cursor_x, cursor_y
        seg_width = len(segment) * FONT_WIDTH_PX
        if cursor_x > x and cursor_x + seg_width > drc.WIDTH:
            cursor_x = x
            cursor_y += FONT_LINE_HEIGHT_PX
        fb.text(segment, cursor_x, cursor_y, color)
        cursor_x += seg_width

    draw_segment("%s (Z=%d) " % (slater.element_symbol(z), z), text_color)
    for n, ell, occ in config:
        segment = "%s%d " % (slater.subshell_label(n, ell), occ)
        r, g, b = atom_cloud.SHELL_RGB[n] if n < len(atom_cloud.SHELL_RGB) else atom_cloud.SHELL_RGB[-1]
        draw_segment(segment, drc.encode_color565(r, g, b))


class AtomPresetState:
    """Everything one loaded element needs to render: Q8 fixed-point
    coordinates and shell-colored, encoded colors -- no resample() (unlike
    orbital_view.PresetState), since atom_cloud's cloud is built once and
    stays static (see atom_cloud.py's module docstring for why: it is a
    mixture of several subshells, and cloud_common's point-turnover only
    knows a single orbital's distribution).
    """

    def __init__(self, z):
        print("atom: loading Z=%d (%s)..." % (z, slater.element_symbol(z)))
        t0 = time.ticks_ms()

        xs, ys, zs, colors_rgb, shells, ells, _signs, config = atom_cloud.build_atom_point_cloud(z, count=N_POINTS)

        self.xs_fx = drc.to_fixed(xs)
        self.ys_fx = drc.to_fixed(ys)
        self.zs_fx = drc.to_fixed(zs)
        self.colors = array.array('H', bytes(2 * len(colors_rgb)))
        for i in range(len(colors_rgb)):
            r, g, b = colors_rgb[i]
            self.colors[i] = drc.encode_color565(r, g, b)

        self.z = z
        self.config = config

        r_ref = atom_cloud.outer_subshell_r_ref(xs, ys, zs, shells, ells, config)
        self.base_scale, self.zoom_amplitude, self.r_ref = atom_cloud.scale_for_atom(r_ref, PIXELS_PER_BOHR)

        print("atom: %s loaded in %dms, scale=%.1f" % (
            slater.element_symbol(z), time.ticks_diff(time.ticks_ms(), t0), self.base_scale))

    def draw_title(self, fb, x, y, text_color):
        _draw_atom_title(fb, x, y, self.z, self.config, text_color)


def run(z=DEFAULT_Z, d=None, detector=None):
    """`d`/`detector` let chooser.py hand this an already-initialized
    display/nudge-detector -- see orbital_view.run()'s matching docstring
    for why. Standalone use (`import atom_view; atom_view.run()`) is
    unaffected, still creates its own.

    Nudging drc.NUDGE_BACK_DIRECTION ('U') returns instead of stepping Z --
    see device_render_common.py's comment on that constant.
    """
    if d is None:
        print("atom: display init...")
        d = display_mod.init()
    print("atom: display ready, Z=1..%d available" % slater.MAX_Z)

    preset = AtomPresetState(z)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = drc.encode_color565(255, 0, 0)
    text_color = drc.encode_color565(255, 255, 255)
    scale_bar_color = drc.encode_color565(210, 210, 210)

    if detector is None:
        detector = drc.init_nudge_detector("element switching")

    angle = 0.0
    tilt_angle = drc._TILT_ANGLE_START
    roll_angle = drc._ROLL_ANGLE_START
    zoom_angle = 0.0
    two_pi = 2 * math.pi

    angle, tilt_angle, roll_angle = drc.fly_over(
        d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
        preset.base_scale * drc.INTRO_START_SCALE_FACTOR, preset.base_scale, drc.INTRO_FRAMES)

    fps_text = "FPS: --"
    frame_count = 0
    fps_window_start = time.ticks_ms()

    zoom_excursion_countdown = drc.next_zoom_excursion_countdown()

    while True:
        # Nudge check: steps the atomic number Z and re-does the fly-over on
        # a detected L/R, same as orbital_view.py's preset switch except
        # clamped to [1, MAX_Z] instead of wrapping -- Z has real endpoints
        # (hydrogen, and this model's practical fallback ceiling), unlike a
        # cyclic preset list. Out-of-range nudges are silently ignored. U
        # returns to the menu (see this function's docstring); D is unused.
        # LOADING_TEXT covers AtomPresetState()'s rebuild so the display
        # doesn't just freeze on the old cloud.
        if detector is not None:
            raw = detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                if direction == drc.NUDGE_BACK_DIRECTION:
                    print("atom: back nudge -- returning to menu")
                    return
                step = drc._NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    new_z = z + step
                    if 1 <= new_z <= slater.MAX_Z:
                        z = new_z
                        fb.fill(0)
                        fb.text(drc.LOADING_TEXT, drc.LOADING_TEXT_POS[0], drc.LOADING_TEXT_POS[1], text_color)
                        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
                        preset = AtomPresetState(z)
                        angle, tilt_angle, roll_angle = drc.fly_over(
                            d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle,
                            roll_angle, preset.base_scale * drc.SWITCH_START_SCALE_FACTOR, preset.base_scale,
                            drc.SWITCH_TRANSITION_FRAMES)

        # Random zoom excursion: pause breathing, fly to a random scale and
        # back (see device_render_common.py's ZOOM_EXCURSION_*). zoom_angle
        # resets to 0 after -- sin(0) == 0 lines up exactly with where the
        # excursion left off. `continue`: this iteration's render already
        # happened inside fly_over(), so skip the normal render/FPS
        # bookkeeping.
        zoom_excursion_countdown -= 1
        if zoom_excursion_countdown <= 0:
            current_scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
            target_scale = preset.base_scale * random.uniform(drc.ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                                drc.ZOOM_EXCURSION_SCALE_MAX_FACTOR)
            angle, tilt_angle, roll_angle = drc.fly_over(
                d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                current_scale, target_scale, drc.ZOOM_EXCURSION_EASE_FRAMES)
            angle, tilt_angle, roll_angle = drc.fly_over(
                d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                target_scale, preset.base_scale, drc.ZOOM_EXCURSION_EASE_FRAMES)
            zoom_angle = 0.0
            zoom_excursion_countdown = drc.next_zoom_excursion_countdown()
            continue

        scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
        drc.render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale)
        preset.draw_title(fb, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], text_color)
        fb.text(fps_text, FPS_TEXT_POS[0], FPS_TEXT_POS[1], text_color)
        drc.draw_scale_bar(fb, scale / atom_cloud.PM_PER_BOHR, "pm", scale_bar_color, text_color)
        d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

        frame_count += 1
        if frame_count >= FPS_UPDATE_INTERVAL:
            now = time.ticks_ms()
            elapsed_ms = time.ticks_diff(now, fps_window_start)
            fps = 1000.0 * frame_count / elapsed_ms if elapsed_ms > 0 else 0.0
            fps_text = "FPS: %.1f" % fps
            print("atom: %s" % fps_text)
            frame_count = 0
            fps_window_start = now

        angle += drc.ANGLE_STEP
        if angle >= two_pi:
            angle -= two_pi
        tilt_angle += drc.TILT_ANGLE_STEP
        if tilt_angle >= two_pi:
            tilt_angle -= two_pi
        roll_angle += drc.ROLL_ANGLE_STEP
        if roll_angle >= two_pi:
            roll_angle -= two_pi
        zoom_angle += drc.ZOOM_ANGLE_STEP
        if zoom_angle >= two_pi:
            zoom_angle -= two_pi
        time.sleep_ms(drc.FRAME_DELAY_MS)


if __name__ == '__main__':
    run()
