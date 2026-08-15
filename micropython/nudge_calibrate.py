"""Standalone calibration aid for nudge.py -- prints which accelerometer
axis/sign fires for each physical nudge, live, with no display/animation
running. Same purpose as examples/corner_calibration/ for the panel's
mirror/color-order: verify a hardware-orientation assumption empirically
before trusting it, rather than guessing (see nudge.py's docstring for why
this is needed here -- a first attempt at capturing all four directions in
one continuous window gave an inconsistent axis/sign per direction).

Usage:
    mpremote connect <port> fs cp micropython/nudge_calibrate.py :
    mpremote connect <port> exec "exec(open('nudge_calibrate.py').read())"

Then nudge the board once in each direction you care about (Left, Right,
Up, Down), pausing ~1s between each so the events don't blur together.
For every nudge that clears the threshold, this prints the dominant axis,
its sign, and the magnitude in g, plus the direction nudge.py's current
table would map it to (or "unmapped" if that axis/sign isn't in the table
yet). Fill in / correct AXIS_SIGN_TO_DIRECTION at the top of nudge.py from
this output. Ctrl-C to stop.
"""

import qmi8658
import nudge

imu = qmi8658.QMI8658()
detector = nudge.NudgeDetector(imu)

print("Nudge calibration running. Nudge the board once per direction,")
print("pausing ~1s between each. Ctrl-C to stop.")

while True:
    raw = detector.poll_raw()
    if raw is not None:
        axis, sign, mag = raw
        mapped = detector.axis_sign_to_direction.get((axis, sign), "unmapped")
        print("axis=%s sign=%+d mag=%.2fg -> %s" % (axis, sign, mag, mapped))
