"""Fake accelerometer for the PC simulator -- exposes the same
`read_accel_g() -> (ax, ay, az)` interface as micropython/qmi8658.py's
QMI8658 class, so micropython/nudge.py's NudgeDetector (imported
unmodified, see micropython_shim.py) can run identically against either
one. The real board's nudge-to-direction mapping is a hardware-orientation
question (see nudge.py's docstring) that simply doesn't apply here -- a
keyboard has no accelerometer axes to be faithful to -- so this maps arrow
keys straight to L/R/U/D via nudge.AXIS_SIGN_TO_DIRECTION's *current*
table, inverted, rather than inventing a separate parallel mapping that
could drift from whatever the real board is calibrated to.

read_accel_g() always includes a resting +1g on Z (gravity, matching
qmi8658.py's raw-includes-gravity convention -- as if the board were lying
flat), plus whatever spike is currently decaying from a recent key press.
The spike decays geometrically each call rather than stepping instantly to
zero, so NudgeDetector's EMA-baseline high-pass filter sees a believable
transient (rise then fade) instead of a step function -- closer to what an
actual physical nudge's accelerometer trace looks like.
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

SPIKE_MAGNITUDE_G = 0.6   # comfortably over nudge.NUDGE_THRESHOLD_G (0.35)
SPIKE_DECAY = 0.5          # fraction of the remaining spike kept per read_accel_g() call


class KeyboardIMU:
    """Bind arrow keys on `tk_widget` (must be focusable/focused to receive
    key events -- see orbital_view_pc.py's canvas.focus_set()) to synthetic
    accelerometer spikes.
    """

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
