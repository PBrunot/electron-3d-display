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

## Multi-electron atoms (approximate)

```sh
python3 pc/atom_main.py [Z]
```

A separate viewer (`atom_view_pc.py`/`atom_main.py`) for elements beyond
hydrogen, approximating any atomic number `Z` (default 6, carbon) as a sum
of hydrogenic subshells: electrons are filled into shells with the n+l
(Madelung) rule plus the known real ground-state exceptions
(`micropython/slater.py`'s `_CONFIG_EXCEPTIONS` — Cr, Cu, Nb, Mo, Ru, Rh,
Pd, Ag, Pt, Au, La, Ce, Gd, Ac, Th, Pa, U, Np, Cm, Lr), and each occupied
subshell gets its own effective nuclear charge via
`slater.z_eff_radial()`: the refined Clementi-Raimondi Hartree-Fock values
(`micropython/slater_cr_zeff.py`, Z<=54) with a fallback to Slater's rules
rescaled by n/n* (Slater's n* consistency) beyond Xe. A FULL subshell is
sampled as spherically symmetric (exact per Unsoeld's theorem); a
partially-filled subshell (e.g. carbon's 2p2) is instead expanded into its
individually-occupied real orbitals per Hund's rule
(`slater.hund_fill_m()`) and sampled with the same per-orbital sampler the
hydrogen presets use — this is what gives partially-filled outer shells
their real, non-spherical shape (see `micropython/atom_cloud.py`'s module
docstring for the full reasoning). Points are colored by shell
(K/L/M/N/...) rather than by wavefunction phase either way.

Accuracy regression checks:

```sh
python3 pc/validate_atoms.py [--strict]
```

Compares the model's valence-shell radii against the Clementi-Raimondi
literature values (period 2 matches within ~3%; periods 3-4 are 1.3-1.5x
over — a known limit of the hydrogenic form, not the Z_eff constants; heavy
Z>54 fallback elements are further off) and runs automated physics checks
(Unsoeld isotropy, Hund anisotropy, Fe 3d<4s ordering, H/He exactness).
See `ATOMS.md` sections 4-5 for the numbers and the methodology note.

**Up/Down arrow keys** change the element (Z) live, with the same fly-over
transition as switching a hydrogen preset. **Mouse wheel** (or **+/- keys**)
zooms in/out — a persistent manual zoom multiplier layered on top of the
automatic zoom-breathing/excursion animation, so it stays applied across
element switches and random zoom excursions alike. No point-turnover shimmer
in this mode yet (the cloud is a static mixture of several subshells — see
`atom_view_pc.py`'s module docstring). Not wired into the ESP32 firmware
path yet, PC-only for now.

This reuses `micropython/orbitals.py`/`pointcloud.py`'s hydrogenic radial
math completely unmodified (the Z-dependence is just the variable
substitution `r -> Z_eff*r` at sampling time, added as new functions
`pointcloud.init_radial_sampler()`/`sample_isotropic_point()`/`radial_mode_radius()`)
— only the angular part (no longer a single `(n, ell, m)` orbital's real
spherical harmonic, but a spherically-averaged subshell) and the
multi-subshell mixing (`atom_cloud.build_atom_point_cloud()`) are new.

## Keeping this in sync with the device

The orchestration layer (orbital math, ranking, scale-from-radii,
point-turnover, `ORBITAL_PRESETS`) lives in `micropython/cloud_common.py`
and is imported by both `orbital_view_pc.py` and the ESP32 firmware
(`micropython/orbital_view.py`) — change it once, both stay in sync. Only
genuinely platform-specific code is duplicated by necessity: the ESP32's
Q8 fixed-point/viper render loop and RGB565 panel encoding have no PC
equivalent, and PC-only extras (bounding sphere/marker, tkinter/PIL
rendering, keyboard nudges) have no device equivalent.
