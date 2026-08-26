"""Hydrogen orbital point-cloud animation for the ESP32-S3 panel. Orbital
math/sampling/ranking/point-turnover logic lives in cloud_common.py, shared
with pc/orbital_view_pc.py. Q8 fixed-point/viper rendering, framebuf/ST7789
blitting, the fly-over/zoom-excursion camera, and nudge/IMU plumbing are
shared with micropython/atom_view.py via device_render_common.py. What's
left here is genuinely orbital-specific: PresetState (phase-by-sign coloring
+ point-turnover), cloud_common.N_POINTS=3000, and the nudge-cycles-a-fixed-
preset-list run() loop.

Rendering: orthographic projection, three-axis tumble (yaw about Y, tilt
about X, roll about Z -- each its own running angle, all at different
non-resonant rates, see device_render_common.py's ANGLE_STEP/TILT_ANGLE_STEP/
ROLL_ANGLE_STEP) applied in sequence each frame, no depth-sort/z-buffer (fine
for a sparse cloud, see CLAUDE.md section 5). Double-buffered via
framebuf.FrameBuffer + ST7789.blit_buffer().

Why three axes, not two: yaw+tilt alone (an earlier version of this code)
still left a real artifact -- a point's SCREEN-X coordinate, after yaw then
tilt, works out to `x*cos(yaw) + z*sin(yaw)` alone (tilt is a rotation about
X, which by definition never changes a point's X coordinate). So any point
near the world Y axis (small x, z -- e.g. the core of a 2p_y or 3d_x2-y2
lobe) stays pinned to the vertical screen centerline for *every* combination
of yaw and tilt, no matter how long you wait -- a persistent streak, not a
transient one. Two single-axis rotations only ever span a 2-parameter
subfamily of SO(3); a third independent axis (roll) is what actually
guarantees no point is invariant in any screen coordinate (verified: adding
roll makes that same near-Y-axis point's screen-X vary freely).

Byte-order gotcha: framebuf's RGB565 storage is little-endian, but the panel
expects big-endian pixels (st7789py's _ENCODE_PIXEL = ">H", needs_swap=False
on this build -- see display.py) -- every color must be pre-byte-swapped
(device_render_common.swap16()) before reaching fb.pixel()/fb.fill_rect(),
or R/B come out swapped.

180-degree prism-viewing offset: applied per-point via display.to_physical()
(inlined in device_render_common.render_points() for speed), NOT baked into
the rotation math, so the camera angle always means the same physical thing
regardless of panel mounting.

Q8 fixed-point + viper (see device_render_common.render_points()): this
MicroPython build's viper can't truncate float->int ("can't convert float to
int") and has no viper `float` type at all, so the per-point rotate/project
loop runs in Q8 fixed-point integers instead of floats -- ~30x faster than
the @micropython.native float version it replaced (~124ms/frame ->
~4ms/frame at 3000 points), reading/writing xs/ys/zs/colors/buf via
ptr32/ptr16 casts (bypasses fb.pixel()'s per-point Python call entirely).

Nudge-to-switch-orbital (see nudge.py): a detected L/R nudge advances/goes
back through cloud_common.ORBITAL_PRESETS; U returns to chooser.py's menu
(see device_render_common.NUDGE_BACK_DIRECTION and this file's run()); D is
currently unused. IMU init is try/except-wrapped -- a board with no QMI8658
wired still runs the animation, just without nudge control.

Point turnover / "buzz" / random zoom excursions: see CULL_FRACTION,
BUZZ_FRACTION (both cloud_common.py) and device_render_common.py's
ZOOM_EXCURSION_* for what each does and why.
"""

import math
import random
import time

import array
import framebuf

import cloud_common
import device_render_common as drc
import display as display_mod

WIDTH = drc.WIDTH
HEIGHT = drc.HEIGHT
CENTER = drc.CENTER

FPS_UPDATE_INTERVAL = 50
FPS_TEXT_POS = (2, 2)


def _encode_color(level, sign, phase_pair):
    # r, g, b = ... then encode_color565(r, g, b), NOT encode_color565(*level_to_rgb(level, sign)) --
    # star-unpacking measured ~4x slower here (matters: called once per point).
    r, g, b = cloud_common.level_to_rgb(level, sign, phase_pair)
    return drc.encode_color565(r, g, b)


class PresetState:
    """Everything one loaded orbital preset needs to render and turn over:
    float coordinates (kept alive for resample()), their Q8 fixed-point
    counterparts (what device_render_common.render_points() actually reads),
    encoded colors, and the cloud_common.ResampleState to keep resampling
    from the same distribution. Bundled here instead of threaded through
    run() as a handful of loose variables.
    """

    def __init__(self, index):
        n, ell, m, label = cloud_common.ORBITAL_PRESETS[index]
        print("orbital: loading preset %d (%s, n=%d l=%d m=%d)..." % (index, label, n, ell, m))
        t0 = time.ticks_ms()

        xs, ys, zs, psi2, signs, sampler, rng, radial_coeff, legendre_coeff = cloud_common.build_point_cloud(n, ell, m)
        levels, psi2_sorted = cloud_common.compute_levels(psi2)

        self.xs, self.ys, self.zs = xs, ys, zs
        self.xs_fx = drc.to_fixed(xs)
        self.ys_fx = drc.to_fixed(ys)
        self.zs_fx = drc.to_fixed(zs)
        # This preset's bright phase-color pair (see cloud_common.ORBITAL_PHASE_COLORS) --
        # kept so resample() re-encodes turned-over points in the same colors.
        self.phase_pair = cloud_common.ORBITAL_PHASE_COLORS[index]
        self.colors = drc.encode_orbital_colors(levels, signs, self.phase_pair)

        self.title = cloud_common.title_for_preset(cloud_common.ORBITAL_PRESETS[index])
        self.base_scale, self.zoom_amplitude, _r_ref = cloud_common.scale_from_radii(xs, ys, zs)
        self.resample_state = cloud_common.ResampleState(
            sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted)

        print("orbital: %s loaded in %dms, scale=%.1f" % (
            label, time.ticks_diff(time.ticks_ms(), t0), self.base_scale))

    def draw_title(self, fb, x, y, text_color):
        """See device_render_common.fly_over()'s docstring for why this is a
        method, not a plain fb.text(preset.title, ...) call at the call
        site: a single orbital title is one uniformly-colored string, unlike
        AtomPresetState's per-shell-colored segments.
        """
        fb.text(self.title, x, y, text_color)

    def resample(self, count):
        """Point turnover (see CULL_FRACTION/CULL_REFRESH_FRAMES): redraw
        `count` points from the same distribution, in place.
        """
        for idx, level, sign in cloud_common.resample_levels(self.resample_state, self.xs, self.ys, self.zs, count):
            if level > cloud_common.COLOR_MAX_LEVEL:
                level = cloud_common.COLOR_MAX_LEVEL  # see resample_levels()'s docstring
            self.xs_fx[idx] = int(self.xs[idx] * drc.FX_SCALE)
            self.ys_fx[idx] = int(self.ys[idx] * drc.FX_SCALE)
            self.zs_fx[idx] = int(self.zs[idx] * drc.FX_SCALE)
            self.colors[idx] = _encode_color(level, sign, self.phase_pair)


def run(d=None, detector=None):
    """`d`/`detector` let chooser.py hand this an already-initialized
    display/nudge-detector instead of each viewer re-running
    display_mod.init()/drc.init_nudge_detector() (re-constructing the
    SPI/I2C bus objects underneath) every time the user nudges back and
    forth between the menu and a viewer -- untested territory worth
    avoiding on real hardware, not just wasteful. Standalone use (`import
    orbital_view; orbital_view.run()`) is unaffected, still creates its own.

    Nudging drc.NUDGE_BACK_DIRECTION ('U') returns instead of stepping the
    preset -- see device_render_common.py's comment on that constant.
    """
    if d is None:
        print("orbital: display init...")
        d = display_mod.init()
    print("orbital: display ready, %d presets available" % len(cloud_common.ORBITAL_PRESETS))

    preset_index = cloud_common.DEFAULT_PRESET_INDEX
    preset = PresetState(preset_index)

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)

    proton_color = drc.encode_color565(255, 0, 0)
    text_color = drc.encode_color565(255, 255, 255)
    scale_bar_color = drc.encode_color565(210, 210, 210)

    if detector is None:
        detector = drc.init_nudge_detector("orbital switching")

    angle = 0.0
    tilt_angle = drc._TILT_ANGLE_START
    roll_angle = drc._ROLL_ANGLE_START
    zoom_angle = 0.0
    two_pi = 2 * math.pi

    # Applied through every fly_over()/render_frame() call below, matching
    # this code's pre-refactor behavior of always computing buzz internally
    # (see device_render_common.fly_over()'s docstring).
    buzz_threshold = int(cloud_common.BUZZ_FRACTION * 65536)

    angle, tilt_angle, roll_angle = drc.fly_over(
        d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
        preset.base_scale * drc.INTRO_START_SCALE_FACTOR, preset.base_scale, drc.INTRO_FRAMES, buzz_threshold)

    fps_text = "FPS: --"
    frame_count = 0
    fps_window_start = time.ticks_ms()

    cull_count = max(1, int(len(preset.xs) * cloud_common.CULL_FRACTION))
    cull_frame_count = 0

    # "Buzz" salt: just needs to change every frame (see render_points()).
    buzz_frame = 0

    zoom_excursion_countdown = drc.next_zoom_excursion_countdown()

    while True:
        # Nudge check: switches presets and re-does the fly-over on a
        # detected L/R. U returns to the menu (see this function's
        # docstring); D is unused. LOADING_TEXT covers PresetState()'s
        # ~1.5-2.5s rebuild so the display doesn't just freeze on the old
        # cloud.
        if detector is not None:
            raw = detector.poll_raw()
            if raw is not None:
                axis, sign, mag = raw
                direction = detector.axis_sign_to_direction.get((axis, sign))
                print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                    axis, sign, mag, direction if direction else "unmapped"))
                if direction == drc.NUDGE_BACK_DIRECTION:
                    print("orbital: back nudge -- returning to menu")
                    return
                step = drc._NUDGE_DIRECTION_STEP.get(direction)
                if step is not None:
                    preset_index = (preset_index + step) % len(cloud_common.ORBITAL_PRESETS)
                    fb.fill(0)
                    fb.text(drc.LOADING_TEXT, drc.LOADING_TEXT_POS[0], drc.LOADING_TEXT_POS[1], text_color)
                    d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)
                    preset = PresetState(preset_index)
                    cull_count = max(1, int(len(preset.xs) * cloud_common.CULL_FRACTION))
                    cull_frame_count = 0
                    angle, tilt_angle, roll_angle = drc.fly_over(
                        d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle,
                        roll_angle, preset.base_scale * drc.SWITCH_START_SCALE_FACTOR, preset.base_scale,
                        drc.SWITCH_TRANSITION_FRAMES, buzz_threshold)

        # Random zoom excursion: pause breathing, fly to a random scale and
        # back (see device_render_common.py's ZOOM_EXCURSION_*). zoom_angle
        # resets to 0 after -- sin(0) == 0 lines up exactly with where the
        # excursion left off. `continue`: this iteration's render already
        # happened inside fly_over(), so skip the normal render/turnover/FPS
        # bookkeeping.
        zoom_excursion_countdown -= 1
        if zoom_excursion_countdown <= 0:
            current_scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
            target_scale = preset.base_scale * random.uniform(drc.ZOOM_EXCURSION_SCALE_MIN_FACTOR,
                                                                drc.ZOOM_EXCURSION_SCALE_MAX_FACTOR)
            angle, tilt_angle, roll_angle = drc.fly_over(
                d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                current_scale, target_scale, drc.ZOOM_EXCURSION_EASE_FRAMES, buzz_threshold)
            angle, tilt_angle, roll_angle = drc.fly_over(
                d, fb, buf, preset, proton_color, text_color, scale_bar_color, angle, tilt_angle, roll_angle,
                target_scale, preset.base_scale, drc.ZOOM_EXCURSION_EASE_FRAMES, buzz_threshold)
            zoom_angle = 0.0
            zoom_excursion_countdown = drc.next_zoom_excursion_countdown()
            continue

        cull_frame_count += 1
        if cull_frame_count >= cloud_common.CULL_REFRESH_FRAMES:
            preset.resample(cull_count)
            cull_frame_count = 0

        scale = preset.base_scale + preset.zoom_amplitude * math.sin(zoom_angle)
        drc.render_frame(fb, buf, preset, proton_color, angle, tilt_angle, roll_angle, scale, buzz_frame,
                          buzz_threshold)
        buzz_frame = buzz_frame + 1 if buzz_frame < 1_000_000 else 0
        preset.draw_title(fb, drc.TITLE_TEXT_POS[0], drc.TITLE_TEXT_POS[1], text_color)
        fb.text(fps_text, FPS_TEXT_POS[0], FPS_TEXT_POS[1], text_color)
        drc.draw_scale_bar(fb, scale / cloud_common.PM_PER_BOHR, "pm", scale_bar_color, text_color)
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
