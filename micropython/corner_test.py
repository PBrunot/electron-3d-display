"""Display smoke test for display.py -- MicroPython counterpart to
examples/corner_calibration/ (see that folder's README for the methodology
this borrows). Runs a full-screen color cycle, then pins one solid color to
each *physical* screen corner, pre-rotating each marker's sprite-space
position via display.to_physical() to compensate for this unit's verified
180 degree prism-viewing offset (see display.py's docstring -- same fix
examples/corner_calibration.ino applies for the C++ port). Kept as a
standalone re-runnable diagnostic -- run this again if the mount changes or
colors/corners ever look wrong.

On this board, `mpremote run <file>` hung after a watchdog reset (looked
like a raw-paste-mode / USB-passthrough interaction, not a MicroPython or
driver bug). What worked reliably instead:
    mpremote connect <port> fs cp st7789py.py display.py corner_test.py :
    mpremote connect <port> exec "exec(open('corner_test.py').read())"
"""

import time

import display
import st7789py as st7789


def color_cycle(d):
    for color in (st7789.RED, st7789.GREEN, st7789.BLUE, st7789.WHITE, st7789.BLACK):
        d.fill(color)
        time.sleep_ms(400)


def _physical_rect(d, px, py, size, color):
    """Fill a `size`x`size` square whose PHYSICAL top-left corner is (px, py)."""
    # to_physical is its own inverse (180 degree flip), so feeding it the
    # square's far corner in physical space gives the square's near corner
    # in sprite space -- that's the (x, y) fill_rect needs.
    sx, sy = display.to_physical(px + size - 1, py + size - 1)
    d.fill_rect(sx, sy, size, size, color)


def corner_test(d):
    d.fill(st7789.BLACK)
    marker = 60
    _physical_rect(d, 0, 0, marker, st7789.RED)
    _physical_rect(d, d.width - marker, 0, marker, st7789.GREEN)
    _physical_rect(d, 0, d.height - marker, marker, st7789.BLUE)
    _physical_rect(d, d.width - marker, d.height - marker, marker, st7789.YELLOW)


def main():
    d = display.init()
    color_cycle(d)
    corner_test(d)
    print(
        "display test done -- expect (physically) RED top-left, GREEN "
        "top-right, BLUE bottom-left, YELLOW bottom-right. If they're not, "
        "the 180 degree offset in display.py no longer matches this unit's "
        "mount -- see examples/corner_calibration/README.md."
    )


main()
