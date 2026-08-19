"""Nudge-gesture detector built on qmi8658.py's accelerometer readings.

A "nudge" here is a short, sharp push on the board in the desk plane -- not
a slow reorientation (tilting the board to look through a different pyramid
face is handled by the physical mount, not this code). The detector tells
the two apart by high-pass filtering: an exponential moving average (EMA)
tracks the slow "where is gravity pointing right now" baseline, and a nudge
is a transient spike in the *deviation* from that baseline, not in the raw
reading.

Axis-to-direction mapping is an explicit table (AXIS_SIGN_TO_DIRECTION
below), not hardcoded math, because which physical accelerometer axis reads
as "left" vs "up" on screen depends on how the board sits in the pyramid rig
-- the same category of hardware assumption CLAUDE.md section 2 flags for
the display's mirror/color order (verify on real hardware, don't assume).

Calibration status: UNVERIFIED. A short on-device capture covering one nudge
per direction (L, R, U, D) in sequence gave inconsistent dominant-axis/sign
per direction -- most likely unclean, non-isolated single-axis pushes in
that capture rather than a driver bug (the same rig reads a sane ~1g at rest
in a static test). The mapping below is therefore a placeholder based on the
*strongest* two events from that capture, not a verified calibration. Run
nudge_calibrate.py (one clean nudge per direction, paused between each) and
edit the table below before relying on direction control.
"""

import time

AXIS_SIGN_TO_DIRECTION = {
    ('y', -1): 'L',
    ('x', -1): 'R',
    ('z', -1): 'U',
    ('z', +1): 'D',
}

EMA_ALPHA = 0.03        # baseline-tracking speed; slow relative to a nudge
NUDGE_THRESHOLD_G = 0.35
COOLDOWN_MS = 400        # ignore further triggers for this long after one fires


class NudgeDetector:
    def __init__(self, imu, threshold_g=NUDGE_THRESHOLD_G, cooldown_ms=COOLDOWN_MS,
                 ema_alpha=EMA_ALPHA, axis_sign_to_direction=None):
        self.imu = imu
        self.threshold_g = threshold_g
        self.cooldown_ms = cooldown_ms
        self.ema_alpha = ema_alpha
        self.axis_sign_to_direction = axis_sign_to_direction or AXIS_SIGN_TO_DIRECTION

        bx, by, bz = imu.read_accel_g()
        self._bx, self._by, self._bz = bx, by, bz
        self._cooldown_until = time.ticks_ms()

    def poll_raw(self):
        """Read the IMU once and return (axis, sign, mag) if a nudge just
        fired (mag in g, axis one of 'x'/'y'/'z', sign +1/-1), else None.
        Unlike poll(), this fires regardless of whether the axis/sign is in
        the direction-mapping table -- used by nudge_calibrate.py so an
        empty/wrong table doesn't hide what actually happened.
        """
        x, y, z = self.imu.read_accel_g()
        a = self.ema_alpha
        self._bx += a * (x - self._bx)
        self._by += a * (y - self._by)
        self._bz += a * (z - self._bz)

        dx, dy, dz = x - self._bx, y - self._by, z - self._bz
        now = time.ticks_ms()
        if time.ticks_diff(now, self._cooldown_until) < 0:
            return None

        mag = (dx * dx + dy * dy + dz * dz) ** 0.5
        if mag < self.threshold_g:
            return None

        adx, ady, adz = abs(dx), abs(dy), abs(dz)
        if adx >= ady and adx >= adz:
            axis, sign = 'x', (1 if dx > 0 else -1)
        elif ady >= adx and ady >= adz:
            axis, sign = 'y', (1 if dy > 0 else -1)
        else:
            axis, sign = 'z', (1 if dz > 0 else -1)

        self._cooldown_until = time.ticks_add(now, self.cooldown_ms)
        return axis, sign, mag

    def poll(self):
        """Read the IMU once and return a direction code ('L'/'R'/'U'/'D')
        if a nudge just fired and maps to a known direction, else None.
        Call this once per frame.
        """
        raw = self.poll_raw()
        if raw is None:
            return None
        axis, sign, _mag = raw
        return self.axis_sign_to_direction.get((axis, sign))
