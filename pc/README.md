# pc/ — PC debug simulator

Runs the same hydrogen-orbital point-cloud animation and nudge-controlled
orbital switching as the ESP32-S3 firmware (`micropython/orbital_view.py`),
in a desktop window instead of on the physical panel — for iterating on the
math/visuals without a round-trip through `mpremote` and real hardware
every time.

See `orbital_view_pc.py`'s module docstring for exactly what's shared with
the device code unmodified — `micropython/cloud_common.py` (orbital math,
sampling, ranking, point-turnover) and `micropython/nudge.py` (gesture
detector) — versus what's genuinely PC-only (the ESP32's Q8 fixed-point/
viper render loop has no PC equivalent — plain floats are fast enough on a
desktop CPU; there's no real accelerometer, so arrow keys stand in for
nudges; the bounding-sphere/marker overlay).

## Requirements

`tkinter` and `Pillow` (PIL), both already available via system packages
on a typical Ubuntu/Debian desktop install — no `pip install` needed there:

```sh
sudo apt-get install python3-tk python3-pil python3-pil.imagetk
```

If you're on a Python build without `tkinter` (e.g. some Homebrew/pyenv
Python installs on this machine lack it — check with
`python3 -c "import tkinter"`), use your system's Python instead:

```sh
/usr/bin/python3 pc/main.py
```

## Run

```sh
python3 pc/main.py
```

A window opens showing the same 240×240 logical view as the device (scaled
up 3× for visibility — see `DISPLAY_SCALE` in `orbital_view_pc.py`), with
the same intro fly-over, rotation/zoom breathing, point-turnover shimmer,
and per-frame "buzz" as the real firmware.

**Arrow keys** (Left/Right/Up/Down) = nudge, same as physically nudging the
board — cycles through `ORBITAL_PRESETS` exactly like the device. The
keyboard-to-direction mapping tracks whatever `micropython/nudge.py`'s
`AXIS_SIGN_TO_DIRECTION` table currently says (see `keyboard_imu.py`), so
if you edit that table for the real board's calibration, arrow keys here
keep meaning the same L/R/U/D without needing a separate edit.

Close the window to quit.

## Keeping this in sync with the device

The orchestration layer (orbital math, ranking, scale-from-radii,
point-turnover, `ORBITAL_PRESETS`) lives in `micropython/cloud_common.py`
and is imported by both `orbital_view_pc.py` and the ESP32 firmware
(`micropython/orbital_view.py`) — change it once, both stay in sync. Only
genuinely platform-specific code is duplicated by necessity: the ESP32's
Q8 fixed-point/viper render loop and RGB565 panel encoding have no PC
equivalent, and PC-only extras (bounding sphere/marker, tkinter/PIL
rendering, keyboard nudges) have no device equivalent.
