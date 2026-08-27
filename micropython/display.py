"""Display bring-up for the Waveshare ESP32-S3-LCD-1.3 board used in this
project (see CLAUDE.md sections 2-3 for hardware/pinout background).

Pins match the verified TFT_eSPI config in platformio.ini (same physical
board, C++ port):
    SPI:       SCLK=40  MOSI=41  CS=39  DC=38  RST=42
    Backlight: GPIO20, active HIGH

Uses machine.SPI (hardware SPI peripheral, not SoftSPI/bit-bang) at 80 MHz, matching the C++
port's LCD_PIXEL_CLOCK_HZ for this target so FPS numbers are comparable between builds. Drop
back to 40_000_000 if this proves unstable.

machine.SPI(2, ...) hangs this specific board on init (watchdog reset,
verified empirically against the real device -- v1.28.0,
ESP32_GENERIC_S3-SPIRAM_OCT firmware); SPI(1) works fine, so that is used
here instead. No MISO pin is wired (the display is write-only), so it is
left unset -- machine.SPI defaults it to an unused GPIO internally without
driving the actual display wiring.

Orientation fix, split between hardware and software -- same split src/render/display.cpp uses
on this identical hardware, and for the same reason: MADCTL with only the MX bit set (0x40),
plus BGR color order, is the clean, non-corrupted part (st7789py.py's custom_rotations
expresses this MX-only entry directly, since none of its four built-in presets is MX-only; it
ORs in the BGR bit itself when color_order=BGR). MX|MY (0xc0) or MY alone (0x80) instead
corrupt the image on this ST7789 variant (quadrants shuffled, not a clean rotation) -- matches
that C++ file's own finding on the same hardware ("combining both mirrors in hardware produced
a broken image on the S3 unit"). Verified on-device that MX alone, with NO software correction,
still leaves sprite-space coordinates landing on the wrong (diagonally opposite) physical
corner -- so unlike the C++ side (which only needs a per-pixel Y-remap on top of its hardware
MX), this driver's MX doesn't get all the way there by itself; the remaining correction is a
full 180-degree flip, applied cheaply (baked into existing per-pixel/per-shape drawing work, not
a separate pass) everywhere content is drawn -- render_points()/render_points_opaque()'s pixel
index, the proton marker's fb.ellipse() position, draw_text_scaled()'s glyph rotation, and
draw_scale_bar()'s line positions (see device_render_common.py). to_physical() below is that
same 180-degree flip for anything drawn directly (e.g. corner_test.py's markers).
"""

from machine import Pin, SPI
import st7789py as st7789

WIDTH = 240
HEIGHT = 240

PIN_SCLK = 40
PIN_MOSI = 41
PIN_CS = 39
PIN_DC = 38
PIN_RST = 42
PIN_BACKLIGHT = 20

SPI_BAUDRATE = 80_000_000

_MADCTL_MX = 0x40
_CUSTOM_ROTATIONS = ((_MADCTL_MX, WIDTH, HEIGHT, 0, 0, False),)


def to_physical(x, y):
    """Map a sprite-space (x, y) to the coordinate that lands in that physical spot on screen --
    the software half of this panel's orientation fix (see module docstring). Use this for
    anything drawn directly (e.g. corner_test.py's markers) rather than raw x/y; the shared
    render path (device_render_common.py) already applies the same flip internally.
    """
    return WIDTH - 1 - x, HEIGHT - 1 - y


def init():
    """Bring up SPI + backlight + the ST7789 driver. Returns the display."""
    backlight = Pin(PIN_BACKLIGHT, Pin.OUT)

    spi = SPI(
        1,
        baudrate=SPI_BAUDRATE,
        polarity=1,
        phase=1,
        sck=Pin(PIN_SCLK),
        mosi=Pin(PIN_MOSI),
    )

    return st7789.ST7789(
        spi,
        WIDTH,
        HEIGHT,
        reset=Pin(PIN_RST, Pin.OUT),
        dc=Pin(PIN_DC, Pin.OUT),
        cs=Pin(PIN_CS, Pin.OUT),
        backlight=backlight,
        color_order=st7789.BGR,
        custom_rotations=_CUSTOM_ROTATIONS,
    )
