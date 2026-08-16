"""Multi-electron atom point-cloud animation for the ESP32-S3 panel -- the
device counterpart of pc/atom_view_pc.py, built the same way orbital_view.py
is: atom_cloud.py supplies the model (multi-electron point cloud, shared
unmodified with the PC viewer), device_render_common.py supplies the Q8
fixed-point/viper rendering, framebuf/ST7789 blitting, fly-over/zoom-
excursion camera, and nudge/IMU plumbing (all shared with orbital_view.py
too). What's left here is genuinely atom-specific: AtomPresetState
(shell-by-n coloring, no point-turnover -- atom_cloud's cloud is static, see
its module docstring), N_POINTS (smaller than the PC viewer's 10000, same
device rendering budget as orbital_view.py's 3000), and a run() loop that
lets nudge step the atomic number Z instead of cycling a fixed preset list.

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

FPS_UPDATE_INTERVAL = 50
FPS_TEXT_POS = (2, 2)


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

        # Short title (element + Z only, not the full electron configuration
        # pc/atom_view_pc.py's wide window shows) -- this panel is 240px, and
        # framebuf's default font has no line-wrapping, so a long
        # configuration string (e.g. iron's "1s2 2s2 2p6 3s2 3p6 3d6 4s2")
        # would run off the right edge.
        self.title = "%s (Z=%d)" % (slater.element_symbol(z), z)

        r_ref = atom_cloud.outer_subshell_r_ref(xs, ys, zs, shells, ells, config)
        self.base_scale, self.zoom_amplitude, self.r_ref = atom_cloud.scale_for_atom(r_ref, atom_cloud.PIXELS_PER_BOHR)

        print("atom: %s loaded in %dms, scale=%.1f" % (
            slater.element_symbol(z), time.ticks_diff(time.ticks_ms(), t0), self.base_scale))


def run(z=DEFAULT_Z):
    print("atom: display init...")
    d = display_mod.init()
    print("atom: display ready, Z=1..%d available" % slater.MAX_Z)

    preset = AtomPresetState(z)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = drc.encode_color565(255, 0, 0)
    text_color = drc.encode_color565(255, 255, 255)
    scale_bar_color = drc.encode_color565(210, 210, 210)

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
        # a detected L/R/U/D, same as orbital_view.py's preset switch except
        # clamped to [1, MAX_Z] instead of wrapping -- Z has real endpoints
        # (hydrogen, and this model's practical fallback ceiling), unlike a
        # cyclic preset list. Out-of-range nudges are silently ignored.
        # LOADING_TEXT covers AtomPresetState()'s rebuild so the display
        # doesn't just freeze on the old cloud.
        if detector is not None:
            raw = detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
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
        fb.text(preset.title, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], text_color)
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
