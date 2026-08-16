"""Keyboard stand-in for the QMI8658 accelerometer.

Exposes the same `read_accel_g() -> (ax, ay, az)` interface as
micropython/qmi8658.py so nudge.py's NudgeDetector (imported unmodified)
runs identically against either. Arrow keys inject a +-SPIKE_MAGNITUDE_G
spike on the axis nudge.py's AXIS_SIGN_TO_DIRECTION table maps to that
direction (inverted), with a resting +1g on Z (gravity, matching the raw
board convention). Spikes decay geometrically per read so the detector's
EMA high-pass sees a believable rise-then-fade transient.
"""

import nudge as _nudge

_DIRECTION_TO_AXIS_SIGN = {direction: axis_sign for axis_sign, direction in
                            _nudge.AXIS_SIGN_TO_DIRECTION.items()}

_KEYSYM_TO_DIRECTION = {
    'Left': 'L',
    'Right': 'R',
    'Up': 'U',
    'Down': 'D',
}

# Spike magnitude is comfortably over nudge.NUDGE_THRESHOLD_G (0.35).
SPIKE_MAGNITUDE_G = 0.6
SPIKE_DECAY = 0.5  # fraction of the remaining spike kept per read_accel_g() call


class KeyboardIMU:
    """Bind arrow keys on `tk_widget` to synthetic accelerometer spikes."""

    def __init__(self, tk_widget):
        self._spike = {'x': 0.0, 'y': 0.0, 'z': 0.0}
        for keysym in _KEYSYM_TO_DIRECTION:
            tk_widget.bind('<KeyPress-%s>' % keysym, self._on_key)

    def _on_key(self, event):
        direction = _KEYSYM_TO_DIRECTION.get(event.keysym)
        if direction is None:
            return
        axis_sign = _DIRECTION_TO_AXIS_SIGN.get(direction)
        if axis_sign is None:
            return  # direction currently unmapped in nudge.py's table
        axis, sign = axis_sign
        self._spike[axis] = sign * SPIKE_MAGNITUDE_G

    def read_accel_g(self):
        x, y, z = self._spike['x'], self._spike['y'], self._spike['z'] + 1.0
        for axis in self._spike:
            self._spike[axis] *= SPIKE_DECAY
        return x, y, z
