"""Display bring-up for the Waveshare ESP32-S3-LCD-1.3 board used in this
project (see CLAUDE.md sections 2-3 for hardware/pinout background).

Pins match the verified TFT_eSPI config in platformio.ini (same physical
board, C++ port):
    SPI:       SCLK=40  MOSI=41  CS=39  DC=38  RST=42
    Backlight: GPIO20, active HIGH

Uses machine.SPI (hardware SPI peripheral, not SoftSPI/bit-bang) at 80 MHz,
matching the C++ port's LCD_PIXEL_CLOCK_HZ for this same target
(src/render/display.cpp's non-CYD branch) -- keeping both builds at the
same SPI clock is what makes micropython/benchmark_test.py's FPS numbers
comparable to src/debug/benchmark_test.cpp's. Drop back to 40_000_000 if
this proves unstable on this driver (SPI(1) below, not SPI(2), see next
paragraph).

machine.SPI(2, ...) hangs this specific board on init (watchdog reset,
verified empirically against the real device -- v1.28.0,
ESP32_GENERIC_S3-SPIRAM_OCT firmware); SPI(1) works fine, so that is used
here instead. No MISO pin is wired (the display is write-only), so it is
left unset -- machine.SPI defaults it to an unused GPIO internally without
driving the actual display wiring.

Panel-mirror + color-order fix: this exact panel is horizontally mirrored in
a way tft.setRotation()'s four proper rotations cannot undo (see CLAUDE.md
"Correzione verificata: mirror pannello e ordine colore" -- proven both
mathematically and empirically in the C++ port / examples/corner_calibration/).
The fix is a raw MADCTL write with only the MX bit set (not MX|MY), plus BGR
(not RGB) color order. st7789py.py's custom_rotations lets us express this
directly instead of picking from its four built-in rotation presets (none of
which is MX-only): one rotation entry with madctl=MX; st7789py itself ORs in
the BGR bit when color_order=BGR.

On top of that mirror fix, this unit's prism mount also needs a plain 180
degree rotation to line the image up with the physical viewing angle --
verified on-device with main.py's corner test: sprite corners were landing
on their diagonally-opposite physical corner (e.g. sprite top-left drawn red
came out physically bottom-right), the same 180 degree offset
examples/corner_calibration/README.md found for the C++ port.

Tried doing that 180 in hardware via MADCTL (MY on top of the existing MX,
using this driver's own built-in 240x240 rotation-table offset of
ystart=80 for madctl=0xc0) -- on-device it produced a corrupted image
(quadrants shuffled asymmetrically, not a clean 180), so this ST7789
variant's RAM-window addressing does not behave like the generic four-way
rotation table assumes for this combination. Reverted to the known-good
MX-only addressing (positions correctly, just needs a software 180) and
apply the 180 degree flip in the drawing code instead, exactly like
examples/corner_calibration.ino does for the C++ port: use
`to_physical(x, y)` below to convert a sprite-space coordinate to the
coordinate that lands in that physical spot, and draw there.
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
_MADCTL_MY = 0x80
_MADCTL_BGR = 0x08
_CUSTOM_ROTATIONS = ((0x00, WIDTH, HEIGHT, 0, 0, False),)


def to_physical(x, y):
    """Map a sprite-space (x, y) to the coordinate that lands in that
    physical spot on screen, given the verified 180 degree prism-viewing
    offset (see module docstring). Use this for anything drawn in
    "intuitive" screen-space (e.g. corner markers) rather than raw x/y.
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
