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
nudges; the bounding-sphere/marker overlay). `viewer_common.py` holds a
second, PC-internal layer shared between `orbital_view_pc.py` and
`atom_view_pc.py` — the render/camera plumbing common to both viewers
(display geometry, tumble, transitions, nucleus/marker/scale-bar/persistence)
— so that layer only needs changing in one place too.

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

A window opens showing the same orbital view as the device at a 480×480
logical resolution (2× the device's 240×240 panel — see `WIDTH`/`HEIGHT` in
`orbital_view_pc.py`), scaled up 2× more for the tkinter window
(`DISPLAY_SCALE`), with the same intro fly-over, rotation/zoom breathing,
point-turnover shimmer, and per-frame "buzz" as the real firmware.

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

On the PC side, `orbital_view_pc.py` and `atom_view_pc.py` share their own
render/camera layer via `viewer_common.py` (display geometry, yaw/tilt/roll
tumble, intro/switch fly-overs, random zoom excursions, nucleus/marker/
scale-bar drawing, phosphor persistence) — change it once there too, instead
of in whichever viewer happened to define it. Genuinely per-viewer state
(the `Preset`/`AtomPreset` classes, `N_POINTS`, the tkinter `App` class and
its input handling) stays in each viewer's own file.

## Constants and tuning reference

All viewer behavior is driven by module-level constants, most of them in
`viewer_common.py` (shared by both viewers), with the rest split between
`orbital_view_pc.py` (hydrogen viewer) and `atom_view_pc.py` (multi-electron
viewer). The code keeps only a one-line comment on each constant; the full
rationale behind the non-obvious values lives here, so the tuning decisions
stay documented without turning the source into prose.

### Display geometry (`viewer_common.py`, except `N_POINTS`)

| Constant | Value | Meaning |
|---|---|---|
| `WIDTH` / `HEIGHT` | 480 / 480 | Logical render resolution (2× the device's 240×240 panel) |
| `CENTER` | `WIDTH // 2` | Screen center |
| `DISPLAY_SCALE` | 2 | The tkinter window is `WIDTH*DISPLAY_SCALE` square; all math stays at `WIDTH`/`HEIGHT` |
| `N_POINTS` (`orbital_view_pc.py`) | 20000 | Sampled points per preset — 3000 on the device; a desktop CPU has the headroom |
| `FRAME_DELAY_MS` | 20 | tkinter `.after()` delay — on a PC this, not rendering, is the real throttle |

### Camera motion (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `ANGLE_STEP` | 0.030 | Yaw (about Y) angular speed per frame |
| `TILT_ANGLE_STEP` | 0.023 | Tilt (about X) angular speed per frame |
| `ROLL_ANGLE_STEP` | 0.017 | Roll (about Z) angular speed per frame |
| `ZOOM_ANGLE_STEP` | 0.016 | Zoom-breathing sine phase step per frame |
| `_TILT_ANGLE_START` | 0.9 | Initial tilt angle |
| `_ROLL_ANGLE_START` | 2.1 | Initial roll angle |

All three rotation axes are required, not decorative: yaw+tilt alone leave a
point's screen-X depending only on its own (x, z) — never on tilt — so
anything near the world Y axis stays pinned to the vertical screen
centerline; roll is what frees it (full derivation in
`micropython/orbital_view.py`'s module docstring). Tilt/roll are kept close
to `ANGLE_STEP` on purpose: with tilt=roll=0 a point's screen-Y depends only
on tilt+roll, not on yaw at all, so if tilt/roll lagged far behind yaw,
axis-aligned lobes (e.g. 3d_x2-y2) would sit still for the first second or
two while yaw visibly spun everything else — reading as "a fixed axis that
doesn't rotate". The three speeds are non-resonant with each other so the
tumble never falls into a short repeating loop. `_TILT_ANGLE_START` /
`_ROLL_ANGLE_START` start away from the degenerate all-zero pose (where yaw
alone can't move axis-aligned lobes at all), so even frame 0 right after
boot isn't axis-locked.

### Intro / orbital-switch transitions (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `INTRO_START_SCALE_FACTOR` | 12.0 | Startup fly-over starts at 12× base scale |
| `INTRO_FRAMES` | 70 | Startup fly-over duration |
| `SWITCH_START_SCALE_FACTOR` | 10.0 | Orbital/Z-switch fly-over starts at 10× base scale |
| `SWITCH_TRANSITION_FRAMES` | 18 | Orbital/Z-switch fly-over duration |

### Random zoom excursions (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `ZOOM_EXCURSION_MIN_INTERVAL_FRAMES` | 150 | Minimum frames between dives |
| `ZOOM_EXCURSION_MAX_INTERVAL_FRAMES` | 400 | Maximum frames between dives |
| `ZOOM_EXCURSION_SCALE_MIN_FACTOR` | 0.35 | Dive target, as a factor of base scale (min) |
| `ZOOM_EXCURSION_SCALE_MAX_FACTOR` | 9.0 | Dive target, as a factor of base scale (max) |
| `ZOOM_EXCURSION_EASE_FRAMES` | 30 | Frames per dive leg (out and back) |

At randomized intervals the camera dives from the current breathing scale to
a randomized target and back, layered on top of the constant sine-wave
breathing so the motion doesn't read as purely mechanical. The max factor is
deliberately deeper than the device's 5.0: the PC has no render-loop budget
to protect, so a dive can go deep enough to feel like passing through
individual points into the electron cloud, not just a bigger breath.

### Bounding sphere + rotation marker (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `BOUNDING_SPHERE_COLOR` | `(70, 70, 90)` | Sphere outline color |
| `MARKER_TEXT` | `"H"` | Marker glyph (the atom viewer passes the element symbol instead) |
| `MARKER_FONT_SIZE` | 15 | Marker glyph size |
| `MARKER_ELEVATION_DEG` | 50.0 | Marker elevation above the horizontal plane |
| `MARKER_COLOR_BEHIND` | `(110, 110, 110)` | Marker/spoke color when rotating away from the viewer |
| `MARKER_COLOR_FRONT` | `(255, 220, 40)` | Marker/spoke color when rotating toward the viewer |

The overlay exists because several presets look close to rotationally
symmetric in plain orthographic projection, so rotation is hard to perceive
from the point cloud alone. The circle sits at radius `r_ref` (the same p90
radius `base_scale` is calibrated against) and never rotates — a sphere's
silhouette is a circle from every angle — so it's a pure size anchor. The
marker is a single reference vector elevated near the pole: 90° would sit
exactly on the Y rotation axis and never move, and 0° would sweep the
equator (tried first — it also visually competed with the title text). It's
what visibly moves each frame, giving an unambiguous read on rotation
direction/speed. The front/back cue is a color shift (vivid warm yellow vs.
flat gray), which reads much stronger than just dimming the same gray-blue.

### Nucleus (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `PROTON_SIZE` | 3 | Nucleus marker size (px) |
| `PROTON_COLOR` | `(255, 0, 0)` | Nucleus marker color |

### Electron point rendering (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `ELECTRON_ALPHA` | 0.5 | Per-point blend fraction toward the point's own color |
| `ENABLE_PERSISTENCE` | True | Fade previous frame instead of clearing (PC-only) |
| `PERSISTENCE_DECAY` | 100 | /256 kept per frame (~0.39) |

`ELECTRON_ALPHA` applies to every sampled electron point; the nucleus is not
affected (one literal particle, not a probability cloud — it stays fully
opaque). Each point blends toward its own color instead of overwriting the
pixel (`new = old + (color − old) * ELECTRON_ALPHA`). A single isolated
point then renders dimmer than its "true" color (blended toward the black
background), while a pixel that several points project onto in the same
frame — common at these projection densities, where 240×240 screen space is
coarse next to 3000–20000 samples — converges toward full brightness as each
subsequent point blends in. Apparent brightness therefore tracks local
sample *density*, not just occupancy, the way a translucent point cloud
reads. `1.0` = opaque (the old direct-overwrite behavior).

Persistence is a PC-only cosmetic: the device stays a hard clear+redraw (no
budget on-device). Fading instead of clearing makes points leave a trailing
glow as they tumble and softens the "buzz" turnover flicker (a skipped point
fades out instead of vanishing). The fade is applied via
`bytes.translate()` — one C-level lookup pass over the whole buffer,
effectively free at 480×480×3 bytes/frame; a per-byte Python loop would not
be. Lower `PERSISTENCE_DECAY` = shorter trails; 256 = never fades.

### Scale bar (`viewer_common.py`)

| Constant | Value | Meaning |
|---|---|---|
| `SCALE_BAR_MARGIN_X` / `_Y` | 8 / 8 | Bottom-left margin (px) |
| `SCALE_BAR_MAX_PX` | 90 | Longest allowed bar |
| `SCALE_BAR_TICK_PX` | 4 | End-tick length |
| `SCALE_BAR_COLOR` | `(210, 210, 210)` | Bar color |

The "nice" round lengths and the length-picking rule live in
`micropython/cloud_common.py` (`SCALE_BAR_CANDIDATES` /
`pick_scale_bar_length()`), shared with the device renderer
(`micropython/orbital_view.py`), so a scale bar reads the same physical
length on both renderers at the same zoom. What's left in the PC code is
PIL-specific geometry and the draw calls. The bar is recomputed from the
frame's live pixels-per-unit every frame, so it tracks the camera's
zoom-breathing/excursion dives rather than only being accurate at rest
scale.

### HUD positions (`viewer_common.py`) and debug switches (`orbital_view_pc.py`)

| Constant | Value | Meaning |
|---|---|---|
| `TITLE_POS` | `(2, 2)` | Title text position |
| `SUBTITLE_POS` | `(2, 12)` | Second-line text position |
| `DEBUG_DISABLE_CULL` | False | Set True to disable point-turnover (resample) |
| `DEBUG_DISABLE_BUZZ` | True | Set False to enable per-frame "buzz" flicker |
| `_NUDGE_DIRECTION_STEP` | `{'R': 1, 'U': 1, 'L': -1, 'D': -1}` | Nudge direction → preset-index step |

The two debug switches exist so the yaw/tilt/roll rotation math can be
confirmed in isolation, with no point-turnover or per-frame flicker muddying
the picture.

### Multi-electron viewer (`atom_view_pc.py`)

| Constant | Value | Meaning |
|---|---|---|
| `N_POINTS` | 10000 | Sampled points per element |
| `DEFAULT_Z` | 6 | Carbon — the simplest element with an interesting (non-full, non-empty) p subshell |
| `ZOOM_FACTOR_MIN` / `_MAX` | 0.15 / 8.0 | Manual zoom multiplier bounds |
| `ZOOM_FACTOR_STEP` | 1.1 | Manual zoom step per wheel notch / keypress |

Manual zoom (mouse wheel / +/- keys) is a persistent multiplier layered on
top of `preset.base_scale`, independent of the automatic
zoom-breathing/excursion animation, and stays applied across element
switches and excursions alike. The step is multiplicative (not additive) so
each notch/keypress feels like the same relative zoom whether already zoomed
in or out.

### Shell-dissection sequence (`atom_view_pc.py`)

| Constant | Value | Meaning |
|---|---|---|
| `DISSECT_TARGET_PX` | 100.0 | On-screen p90 radius each shell's disc is zoomed to fill |
| `DISSECT_SHADE_GRAY` | `(70, 70, 70)` | Flat gray for every non-active shell's points |
| `ACTIVE_SUBSHELL_ALPHA` | 1.0 | Opaque — the exploded subshell ignores `ELECTRON_ALPHA` |
| `DISSECT_CLIP_OPEN` | 0.0 | Clip threshold hiding rotated-z > 0 (the half facing the camera) |
| `DISSECT_CLIP_CLOSED` | 1.0e6 | No real point exceeds it — nothing hidden |
| `DISSECT_ORIENT_FRAMES` | 40 | Frames to ease the cut open (still tumbling) |
| `DISSECT_ZOOM_FRAMES` | 40 | Frames to ease scale from one shell to the next |
| `DISSECT_HOLD_SECONDS` | 2 | Real-time pause per shell, label shown, still tumbling |
| `DISSECT_CLOSE_FRAMES` | 80 | Frames to ease the cut shut on return |
| `DISSECT_FRAME_DELAY_S` | `FRAME_DELAY_MS / 1000.0` | Per-frame pacing of every dissection leg |

The clip is applied in camera space every frame, so which physical points
fall inside it changes continuously as the atom turns — the way a peeled
sphere looks mid-spin. `DISSECT_FRAME_DELAY_S` paces every leg of the
sequence to the same rotation speed normal viewing uses: unlike the fly-over
transitions (no delay, run as fast as the CPU renders), the sequence needs a
real-time `DISSECT_HOLD_SECONDS` pause to be legible, so all legs pace
themselves the same way for a consistent rotation speed throughout. The
dissection view also deliberately has no persistence — with the clip plane
re-applied fresh to a continuously rotating cloud, a trailing glow would
smear the cut edge instead of reading as motion.

### Keyboard IMU (`keyboard_imu.py`)

| Constant | Value | Meaning |
|---|---|---|
| `SPIKE_MAGNITUDE_G` | 0.6 | Spike amplitude per keypress — comfortably over `nudge.NUDGE_THRESHOLD_G` (0.35) |
| `SPIKE_DECAY` | 0.5 | Fraction of the remaining spike kept per `read_accel_g()` call |

The real board's nudge-to-direction mapping is a hardware-orientation
question (see `micropython/nudge.py`'s docstring) that doesn't apply to a
keyboard — there are no accelerometer axes to be faithful to — so arrow keys
map straight to L/R/U/D via `nudge.AXIS_SIGN_TO_DIRECTION`'s *current*
table, inverted, rather than a separate parallel mapping that could drift
from whatever the real board is calibrated to. `read_accel_g()` always
includes a resting +1g on Z (gravity, matching `qmi8658.py`'s
raw-includes-gravity convention — as if the board were lying flat) plus the
decaying spike. The spike decays geometrically each call rather than
stepping instantly to zero, so `NudgeDetector`'s EMA-baseline high-pass
filter sees a believable rise-then-fade transient instead of a step function
— closer to what an actual physical nudge's accelerometer trace looks like.

### MicroPython shim (`micropython_shim.py`)

`@micropython.native` / `@micropython.viper` are compiler hints on the
device — decorator *syntax*, not runtime attribute lookups — so
`micropython/orbitals.py` and `micropython/pointcloud.py` never
`import micropython` themselves. CPython evaluates them as ordinary
decorator expressions and needs a resolvable name, so the shim injects a
`micropython` object into `builtins` (the only namespace CPython's
name-resolution fallback reaches without editing those files); both
decorators are identity functions on a PC, where CPython bytecode is already
far faster than interpreted MicroPython on an ESP32. The shim also patches
`time.ticks_ms()` / `ticks_diff()` / `ticks_add()` (used by
`nudge.py`'s cooldown timer) — CPython's `time` module has no such API.
They're plain monotonic-time arithmetic, since CPython's `monotonic()`
never wraps within any timeframe this program runs.

### Validation harness (`validate_atoms.py`)

| Constant | Value | Meaning |
|---|---|---|
| `ISOTROPY_SAMPLES` | 20000 | Points sampled per isotropy/anisotropy check |
| `ISOTROPY_TOL` | 0.05 | Max \|<x²>/<r²> − 1/3\| tolerated |
| `H_AND_HE_TOL` | 0.03 | Model/lit radius ratio tolerance for H and He |
| `RADIAL_MODE_RESOLUTION` | 20001 | Scan density for the radial mode |
| `DEFAULT_RATIO_MIN` / `_MAX` | 0.5 / 2.0 | `--strict` gate ratio bounds |

`RADIAL_MODE_RESOLUTION` is the mode-scan density used by
`pointcloud.radial_mode_radius()`: the mode is stable to well under 1% at
this density (the physics errors the harness measures are 5–400%), and it
keeps `--all` (Z=1..118) usable in a couple of seconds. The two key
definitions the harness enforces are: "valence subshell" = the highest-l
subshell among the highest-n occupied ones (e.g. carbon's 2p, iron's 4s),
and "model radius" = the mode of r²·R(z_eff·r)² using the same
`z_eff_radial()` the point cloud is built with (Clementi-Raimondi where the
table covers the subshell, else Slater's rules rescaled by n/n*).

