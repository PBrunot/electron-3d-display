"""Minimal MicroPython driver for the QMI8658 6-axis IMU (accelerometer +
gyroscope) on the Waveshare ESP32-S3-LCD-1.3 (CLAUDE.md section 2: SDA=47,
SCL=48). Only the accelerometer is enabled/read -- nudge.py's gesture
detector only needs linear acceleration, not gyro rate.

Register map and init sequence verified live on this exact board
(2026-08-15, via mpremote): i2c.scan() found the sensor at 0x6B, WHO_AM_I
(register 0x00) read back 0x05 as expected. Addresses/bit layout otherwise
match the open-source CircuitPython_QMI8658C driver
(github.com/tkomde/CircuitPython_QMI8658C) -- ported rather than re-derived
from the datasheet from scratch, then cross-checked against the live
WHO_AM_I read and a live accelerometer sanity read (~1g total magnitude at
rest) before trusting it, same "verify on real hardware" methodology as
CLAUDE.md section 2's display mirror/color-order correction.

Gotcha (also caught live, on the first sanity read): MicroPython's
int.from_bytes() has no `signed` parameter -- unlike CPython, it silently
ignores a third positional/keyword argument and always returns an unsigned
value. Passing raw unsigned words through the +/-4g scale factor read as
~11g at rest before this was noticed; _signed16() below does the two's
complement conversion by hand instead.
"""

from machine import I2C, Pin

_WHO_AM_I = 0x00
_CTRL1 = 0x02
_CTRL2 = 0x03
_CTRL7 = 0x08
_ACCEL_OUT = 0x35

_EXPECTED_WHO_AM_I = 0x05

# CTRL2 = accel range in bits [6:4] | ODR in bits [3:0] (CircuitPython_QMI8658C
# convention, confirmed by reading CTRL2 back after the write on-device).
_RANGE_4G = 1
_RANGE_4G_SCALE = 8192.0  # LSB/g at +/-4g full scale
_ODR_250HZ = 5

DEFAULT_ADDR = 0x6B
DEFAULT_SDA = 47
DEFAULT_SCL = 48


class QMI8658:
    def __init__(self, i2c=None, addr=DEFAULT_ADDR, sda=DEFAULT_SDA, scl=DEFAULT_SCL, freq=400_000):
        self.i2c = i2c if i2c is not None else I2C(0, scl=Pin(scl), sda=Pin(sda), freq=freq)
        self.addr = addr

        who = self.i2c.readfrom_mem(self.addr, _WHO_AM_I, 1)[0]
        if who != _EXPECTED_WHO_AM_I:
            raise OSError(
                "QMI8658 WHO_AM_I mismatch: got 0x%02x, expected 0x%02x" % (who, _EXPECTED_WHO_AM_I)
            )

        self.i2c.writeto_mem(self.addr, _CTRL1, bytes([0x60]))
        self.i2c.writeto_mem(self.addr, _CTRL2, bytes([(_RANGE_4G << 4) | _ODR_250HZ]))
        self.i2c.writeto_mem(self.addr, _CTRL7, bytes([0x01]))  # accel enable, gyro left off

    @staticmethod
    def _signed16(v):
        return v - 65536 if v >= 32768 else v

    def read_accel_g(self):
        """Return (ax, ay, az): linear acceleration in g, board-local axes
        (includes gravity -- this is raw accelerometer output, not a
        gravity-subtracted reading).
        """
        d = self.i2c.readfrom_mem(self.addr, _ACCEL_OUT, 6)
        ax = self._signed16(d[0] | (d[1] << 8))
        ay = self._signed16(d[2] | (d[3] << 8))
        az = self._signed16(d[4] | (d[5] << 8))
        s = _RANGE_4G_SCALE
        return ax / s, ay / s, az / s
