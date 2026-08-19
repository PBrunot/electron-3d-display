"""Device counterpart of pc/launcher.py: an initial menu screen with a
randomly picked tumbling preset playing behind it, then hands off to
whichever viewer the user picks. Nudging back up from within either viewer
returns here -- see device_render_common.NUDGE_BACK_DIRECTION and
orbital_view.py/atom_view.py's run().

    mpremote connect <port> exec "import chooser; chooser.run()"

Run this instead of the current main.py default (orbital_view.run()
directly) to get the menu -- see main.py for the boot wiring.

No keyboard on this hardware, only IMU tilt nudges (Left/Right/Up/Down, see
nudge.py) -- and both viewers already use L/R to cycle presets/elements and
U to mean "back" (see device_render_common.py's _NUDGE_DIRECTION_STEP/
NUDGE_BACK_DIRECTION), so there is no gesture left over from those to also
mean "select" here. Instead, each of the menu's two choices has its OWN
dedicated nudge direction that launches it immediately (U = Hydrogen
Orbitals, D = Element Explorer) -- consistent with how a nudge already means
"take this action now" everywhere else in this project, not "move a cursor,
then confirm separately". L/R are unused at the menu (available for a
future third choice or a manual backdrop preview).

This is the ONLY place that calls display_mod.init()/drc.init_nudge_detector()
-- both viewers accept an already-initialized `d`/`detector` instead of
creating their own each time they're (re)entered, since re-running those on
every back-and-forth would mean repeatedly re-constructing the SPI/I2C bus
objects underneath -- untested territory on this hardware, not just
inefficient, so best avoided by construction.
"""

import math
import random
import time

import framebuf

import cloud_common
import device_render_common as drc
import display as display_mod
import slater

import atom_view
import orbital_view

WIDTH = drc.WIDTH
HEIGHT = drc.HEIGHT
CENTER = drc.CENTER

# Label positions as fractions of HEIGHT, matching pc/launcher.py's
# BUTTON_*_FRAC/TITLE_GAP_FRAC -- same "1/3 and 2/3 height, backdrop stays
# visible in between" layout idea as the PC/web menus, just framebuf text
# labels instead of drawn buttons (this hardware has no pointer to click
# anyway, so there's nothing a drawn button outline would add).
_Y_THIRD = HEIGHT // 3
_Y_TWO_THIRD = HEIGHT * 2 // 3
TITLE_POS = (10, _Y_THIRD - 34)
ORBITALS_LABEL_POS = (10, _Y_THIRD)
ATOM_LABEL_POS = (10, _Y_TWO_THIRD)

TITLE_TEXT = "Choose a viewer"
ORBITALS_LABEL = "Nudge UP: Orbitals"
ATOM_LABEL = "Nudge DOWN: Elements"

TITLE_COLOR = drc.encode_color565(180, 180, 180)
ORBITALS_COLOR = drc.encode_color565(255, 220, 40)
ATOM_COLOR = drc.encode_color565(120, 200, 255)
PROTON_COLOR = drc.encode_color565(255, 0, 0)


def _random_backdrop():
    """Random tumbling preset for the menu's backdrop -- a hydrogen orbital
    or a random element, re-rolled each time the menu is (re)shown,
    mirroring pc/launcher.py's ChooserScene.
    """
    if random.random() < 0.5:
        index = random.randrange(len(cloud_common.ORBITAL_PRESETS))
        print("chooser: backdrop = orbital preset %d" % index)
        return orbital_view.PresetState(index)
    z = random.randint(1, slater.MAX_DISPLAY_Z)
    print("chooser: backdrop = element Z=%d" % z)
    return atom_view.AtomPresetState(z)


def run():
    print("chooser: display init...")
    d = display_mod.init()
    detector = drc.init_nudge_detector("menu selection")

    buf = bytearray(WIDTH * HEIGHT * 2)
    fb = framebuf.FrameBuffer(buf, WIDTH, HEIGHT, framebuf.RGB565)
    two_pi = 2 * math.pi

    while True:
        preset = _random_backdrop()
        scale = preset.base_scale
        angle = random.uniform(0, two_pi)
        tilt_angle = drc._TILT_ANGLE_START
        roll_angle = drc._ROLL_ANGLE_START

        print("chooser: showing menu")
        while True:
            if detector is not None:
                raw = detector.poll_raw()
                if raw is not None:
                    axis, sign, mag = raw
                    direction = detector.axis_sign_to_direction.get((axis, sign))
                    print("nudge: axis=%s sign=%+d mag=%.2fg -> %s" % (
                        axis, sign, mag, direction if direction else "unmapped"))
                    if direction == 'U':
                        orbital_view.run(d, detector)
                        break  # viewer returned (back nudge) -- re-roll and show the menu again
                    if direction == 'D':
                        atom_view.run(atom_view.DEFAULT_Z, d, detector)
                        break

            drc.render_frame(fb, buf, preset, PROTON_COLOR, angle, tilt_angle, roll_angle, scale)
            fb.text(TITLE_TEXT, TITLE_POS[0], TITLE_POS[1], TITLE_COLOR)
            fb.text(ORBITALS_LABEL, ORBITALS_LABEL_POS[0], ORBITALS_LABEL_POS[1], ORBITALS_COLOR)
            fb.text(ATOM_LABEL, ATOM_LABEL_POS[0], ATOM_LABEL_POS[1], ATOM_COLOR)
            d.blit_buffer(buf, 0, 0, WIDTH, HEIGHT)

            angle += drc.ANGLE_STEP
            if angle >= two_pi:
                angle -= two_pi
            tilt_angle += drc.TILT_ANGLE_STEP
            if tilt_angle >= two_pi:
                tilt_angle -= two_pi
            roll_angle += drc.ROLL_ANGLE_STEP
            if roll_angle >= two_pi:
                roll_angle -= two_pi
            time.sleep_ms(drc.FRAME_DELAY_MS)


if __name__ == '__main__':
    run()
