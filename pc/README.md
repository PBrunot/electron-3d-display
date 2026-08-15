# pc/ — PC debug simulator

Runs the same hydrogen-orbital point-cloud animation and nudge-controlled
orbital switching as the ESP32-S3 firmware (`micropython/orbital_view.py`),
in a desktop window instead of on the physical panel — for iterating on the
math/visuals without a round-trip through `mpremote` and real hardware
every time.

See `orbital_view_pc.py`'s module docstring for exactly what's shared with
the device code unmodified (the orbital math in `micropython/orbitals.py`
and `micropython/pointcloud.py`, plus `micropython/nudge.py`'s gesture
detector) versus what's re-implemented for the PC (the ESP32's Q8
fixed-point/viper render loop has no PC equivalent — plain floats are fast
enough on a desktop CPU; there's no real accelerometer, so arrow keys
stand in for nudges).

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

`orbital_view_pc.py` duplicates the ESP32 firmware's orchestration layer
(`build_point_cloud()`, `point_colors()`, `_scale_from_radii()`, the
resample/"buzz" logic, `ORBITAL_PRESETS`) rather than importing it, because
`micropython/orbital_view.py` unconditionally imports ESP32-only hardware
drivers (`framebuf`, `display`, `st7789py`) at module scope and can't be
imported under CPython at all. If you change one of those algorithms on
the device, mirror the change here (they're written to read as a close
line-by-line match specifically so a diff is easy) — or do the module
split that would let both share one file, noted in `orbital_view_pc.py`'s
docstring as the "correct" fix this tool didn't attempt, to avoid touching
already-verified, deployed device code in the same change that added it.
