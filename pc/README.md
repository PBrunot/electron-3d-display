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

`numpy` is used for the vectorized render fast path (what makes 30 fps
possible at this point count); without it the viewers fall back to a
pure-Python render loop (slower, but fully functional). On Debian/Ubuntu
`python3-numpy` is a system package; on other installs
`pip install numpy` works.

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

A window opens on a 2s boot splash -- the atomic-cube image
(`img/atomic_cube.jpg` / `data/atomic_cube.jpg`, the same image the device decodes
on-device from the "storage" partition in `src/render/splash_bitmap.cpp`; the PC
just loads the JPEG directly) --
then a chooser screen (`pc/launcher.py`) over that SAME static image as its
background: an electric-blue "ATOM CUBE" title and two bigger options, "UP:
Orbitals" vs "DOWN: Elements" (port of the device's chooser: same fixed
splash background, no animation needed; any key/click skips the splash).
**Up/Down/Left/Right** or click to select, **Enter** to confirm. **Escape**
inside either viewer returns here -- one shared window the whole time, no new
windows opened or closed when switching. To jump straight into a specific
element from the command line instead, skipping the chooser:
`python3 pc/atom_main.py [Z]`.

Once inside the orbital viewer, it shows the same view as the device at a
480×480 logical resolution (2× the device's 240×240 panel — see
`WIDTH`/`HEIGHT` in `orbital_view_pc.py`), scaled up 2× more for the
tkinter window (`DISPLAY_SCALE`), with the same intro fly-over,
rotation/zoom breathing, point-turnover shimmer, and per-frame "buzz" as
the real firmware.

**Arrow keys** (Left/Right/Up/Down) = nudge, same as physically nudging the
board — cycles through `ORBITAL_PRESETS` exactly like the device. The
keyboard-to-direction mapping tracks whatever `micropython/nudge.py`'s
`AXIS_SIGN_TO_DIRECTION` table currently says (see `keyboard_imu.py`), so
if you edit that table for the real board's calibration, arrow keys here
keep meaning the same L/R/U/D without needing a separate edit.

Close the window to quit.

## README screenshots

`pc/screenshot.py` renders the same frames the two viewers show - same
seeded point clouds, same camera pose, same overlays - straight to PNG/GIF
in the repo's `img/` folder, without opening a window. Used for the GitHub
README's visual illustrations:

```sh
python3 pc/screenshot.py                      # everything below
python3 pc/screenshot.py --orbitals           # hydrogen orbital presets + 4x4 gallery
python3 pc/screenshot.py --atoms 6 26         # multi-electron atoms for those Z values
python3 pc/screenshot.py --dissect 20         # shell-dissection journey for that Z
python3 pc/screenshot.py --orbitals-gif       # animated orbital showcase (img/orbitals.gif)
python3 pc/screenshot.py --dissect-gif 20     # animated dissection journey (img/dissect_Ca.gif)
```

Outputs: one `orbital_*.png` per `ORBITAL_PRESETS` entry, one `atom_*.png`
per element (auto-zoomed so the outer reference sphere fills the frame like
the orbital presets do), the shell-dissection journey per element as
still frames (`dissect_<sym>_<n>_<subshell>.png`, outermost to innermost,
plus the full-cloud wide shots at the start and end), an `atom_gallery.png`
arranged like a PERIODIC TABLE (every element H..Xe, Z=1..54 -- the
Clementi-Raimondi-validated range -- at its (period, group) cell, all at
the SAME physical scale, no per-element zoom, so the real size trend
across periods/groups reads directly), an `orbital_gallery.png`, and a
`hero.png` strip. The dissection journey has NO clip plane: unlike the
app's debug D-key view, the full cloud stays visible in every frame (the
camera just zooms through the subshells, highlighting each in turn) and
the bounding circle is plain gray, not shell-colored. All montages (`hero.png`,
galleries, journey strips) are assembled at FULL source resolution - each
cell is the 480x480 screenshot itself, no downscaling.

Two animated GIFs replay the ORIGINAL viewer loops -- the same per-source-
frame rotation/zoom-breathing/point-turnover cadence as the live app and
the same eased dissection zoom legs with the source's own frame counts
(the journey GIF also drops the clip plane, matching the stills) -- at
FULL 480x480 resolution, the same as the still PNGs. To keep files
README-sized the GIF writes only every GIF_SUBSAMPLE-th source frame and
plays them back at the same wall-clock speed, so the motion is temporally
identical to the source at a lower frame rate (default GIF_FPS = 12 ->
12.5 fps actual). Tunables: ORBITAL_GIF_SECONDS (1s per orbital),
DISSECT_GIF_HOLD_SECONDS (1s per dissected layer), and GIF_FPS (lower =
smaller file). Current sizes: `img/orbitals.gif` ~9 MB (283 frames, ~22s,
all 16 presets with the nudge-switch fly-overs), `img/dissect_Ca.gif`
~1.4 MB (148 frames, ~11s, calcium journey without clipping).

All frames are deterministic (the samplers use fixed seeds) and reuse the
viewers' own render/overlay functions - nothing is reimplemented for
screenshots.

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
in this mode (the cloud is a static mixture of several subshells — see
`atom_view_pc.py`'s module docstring).

The device counterpart, `micropython/atom_view.py`, has the same model and
render loop (nudge steps Z instead of Up/Down, clamped to `[1, MAX_Z]`
instead of wrapping) but no manual zoom or shell-dissection view — those are
PC debug extras with no framebuf equivalent worth the effort. It's not the
default boot animation (CLAUDE.md's roadmap is single-electron hydrogen
orbitals); see that module's docstring for how to run it standalone.

This reuses `micropython/orbitals.py`/`pointcloud.py`'s hydrogenic radial
math completely unmodified (the Z-dependence is just the variable
substitution `r -> Z_eff*r` at sampling time, added as new functions
`pointcloud.init_radial_sampler()`/`sample_isotropic_point()`/`radial_mode_radius()`)
— only the angular part (no longer a single `(n, ell, m)` orbital's real
spherical harmonic, but a spherically-averaged subshell) and the
multi-subshell mixing (`atom_cloud.build_atom_point_cloud()`) are new.

### Screened-potential (HFS) model — `--model hfs` (experimental)

```sh
python3 pc/atom_main.py 26 --model hfs        # iron with the new radial model
python3 pc/validate_atoms.py --model hfs --strict --all
python3 pc/hfs_solver.py --zmin 1 --zmax 118 --alpha 0.6666667 --out pc/hfs_tables.npz
```

The hydrogenic-Z_eff approximation above is replaced, on request, by the
classic self-consistent screened-potential model: every occupied (n, l)
subshell is an eigenstate of a single central potential built from the
atom's own electron density (nuclear + electron-electron + Slater exchange
with the Latter -1/r cutoff), solved offline by `pc/hfs_solver.py`
(Hartree-Fock-Slater; log-grid eigenvalue problem, ARPACK shift-invert) and
tabulated as u(r) = r R(r) per subshell. `--model hfs` makes the PC viewer
and the validation harness use those tables instead of the hydrogenic
substitution (the shared samplers in `micropython/pointcloud.py` gained
table-fed paths; the hydrogenic default is untouched). `--relativistic`
(PC-only tables so far) replaces the final states with solutions of the
radial Dirac equation (`pc/dirac_solver.py`) — the s/p contraction for
Z >= 55. See `pc/screened_potential_model.md` for the design and the
validation numbers, and `ATOMS.md` section 5 for the accuracy discussion
(α=2/3, the exchange that matches the NIST LDA eigenvalues to <0.7 eV,
with SCF density mixing 0.35 to keep the transition-metal 3d/4s ordering
physical).

## Keeping this in sync with the device

The orchestration layer (orbital math, ranking, scale-from-radii,
point-turnover, `ORBITAL_PRESETS`) lives in `micropython/cloud_common.py`
and is imported by both `orbital_view_pc.py` and the ESP32 firmware
(`micropython/orbital_view.py`) — change it once, both stay in sync. The
multi-electron atom model (`micropython/atom_cloud.py`) is imported the same
way by `atom_view_pc.py` and `micropython/atom_view.py`. Only genuinely
platform-specific code is duplicated by necessity: the ESP32's Q8
fixed-point/viper render loop and RGB565 panel encoding have no PC
equivalent, and PC-only extras (bounding sphere/marker, tkinter/PIL
rendering, keyboard nudges) have no device equivalent.

On the device side, `orbital_view.py` and `atom_view.py` share their own
render/camera layer the same way the two PC viewers share
`viewer_common.py`: `micropython/device_render_common.py` holds the Q8
fixed-point/viper point renderer, framebuf blitting, fly-over/zoom-excursion
camera, and nudge/IMU setup common to both.

On the PC side, `orbital_view_pc.py` and `atom_view_pc.py` share their own
render/camera layer via `viewer_common.py` (display geometry, yaw/tilt/roll
tumble, intro/switch fly-overs, random zoom excursions, nucleus/marker/
scale-bar drawing, phosphor persistence) — change it once there too, instead
of in whichever viewer happened to define it. `atom_view_pc.py`'s `AtomPreset`
class and the shell-dissection Phase 0-5 plan live in
`atom_dissection_common.py` instead, shared with `web/py/web_atom.py`
(fetched into Pyodide the same way `render_core.py` already is) — the device
has no dissection feature, so that module is PC/web-only. Genuinely
per-viewer state (`Preset`, `N_POINTS`, the tkinter `App` class and its input
handling) stays in each viewer's own file.

## Ported device polish (2026-08-17)

The ESP32 C++ work from 2026-08-17 (src/ commits ae07963 + 59ddea3) was
ported here idea-for-idea, re-implemented per platform:

- **Brighter electrons / longer persistence**: `ELECTRON_ALPHA` 0.8→0.92,
  `PERSISTENCE_DECAY` 100→150 then 120 (see the constants sections above).
- **Sign-based phase coloring, classic vibrant orange/blue**: every one of
  the 16 orbital presets colors positive/negative lobes of psi_real with
  the same classic vibrant orange/blue pair ("all orbitals should be
  colored according to sign of psi_Real with the classical blue/orange
  vibrant colors", 2026-08-17) -- a consistent sign→color mapping across
  the whole library, rather than each preset having its own distinguishing
  hue (an earlier, since-superseded scheme). Defined once in
  `micropython/cloud_common.py` (`ORBITAL_PHASE_COLORS`, used by PC + web),
  mirrored in `src/physics/orbital_library.h`'s `OrbitalDescriptor` for the device.
  The dim-point floor was raised too (`COLOR_MIN_LEVEL` 60→80, and the
  C++'s `kOrbitalColorMinLevel`).
- **Proton always visible**: bigger (14px) and drawn on top of the cloud
  every frame.
- **Bigger scale bar** with a 2× label font.
- **Chooser**: static atomic-cube background (no tumbling preset), no
  "ATOM CUBE" title text over the image, plain "Orbitals"/"Atoms" option
  names (the PC navigates with arrow keys, not gestures — the device keeps
  its "UP: Orbitals"/"DOWN: Elements" wording), plus a 2s boot splash before
  it.
- **Orbital quantum-number reveal**: on every orbital switch (and the idle
  jump), n → n l → n l m is revealed one stage at a time (~0.55s each, +0.5s
  on the final) over a backdrop image. The device decodes data/orbitals.jpg
  once per reveal (render/jpeg_bg.h) and only repaints the text band per
  stage. The PC instead draws the Schrödinger equation and `psiReal()`
  directly as text (Unicode font, ASCII fallback) -- no image asset, not
  kept in sync with the device's backdrop.
- **Orbital zooms 1.5× slower** (intro/switch/excursion frames ×1.5,
  breathing zoom step ÷1.5), scoped to the orbital viewer only.
- **Idle auto-advance** (both viewers): 60s without input jumps to a random
  different orbital/element using the same animation as manual navigation.
  In the atom viewer the idle timeout coin-flips between dissecting the
  current element (at most once per element) and jumping.
- **Element-switch intro**: the Italian element name slides in from the
  right over a big pale symbol watermark, holds, then flashes on/off once at
  0.5Hz. Layout per feedback: the name sits at 2/3 of the canvas height and
  the "Z=xx" caption in the upper 1/3, so the name reads clearly.
- **Dissection intro card**: "Configurazione / elettronica / <nome>" over a
  tiled dim "e-" backdrop, held ~0.9s before the sequence starts.
- **Dissection HUD**: big subshell label ("2p") + plain-size caption
  ("Shell 2p (2/5)") + a small "<occ>e-" note in the top-right corner
  (was a verbose single subtitle line).
- **Dissection pacing**: shell-to-shell hops take ~2× as long
  (`DISSECT_ZOOM_SLOWDOWN`); the open/close legs keep their own constants.
- **Dissection abort**: any movement (arrow keys, wheel, +/-, D) during a
  dissection closes it and returns to the full element.
- **30 fps**: the per-point Python render loop was the whole bottleneck
  (~140ms/frame at 20000 points). With numpy installed the shared render
  core (`pc/render_core.py` -- also imported by the web port under Pyodide,
  see below) is fully vectorized -- rotation, projection, 2×2 blocks, and
  the exact sequential alpha blend (per-pixel "rounds", same semantics as
  the Python loop, verified pixel-equal within rounding) -- bringing a
  5000-point frame to ~30ms (measured ~36 fps on the dev machine). The
  pure-Python loops remain as the no-numpy fallback.
- **Web parity via the shared render core**: the browser demo
  (web/py/web_common.py) imports the SAME `pc/render_core.py` module (it's
  fetched into Pyodide per web/index.html's PY_FILES, alongside the already-
  shared micropython/ model files), so the web now shows the same look as
  the PC -- 2×2 electrons at alpha 0.92, the 14px on-top nucleus, the
  doubled scale bar, and the per-orbital colors (which already flowed
  through `cloud_common.ORBITAL_PHASE_COLORS`). Its chooser got the same
  treatment (plain "Orbitals"/"Atoms", no title; it keeps its tumbling
  backdrop since Pyodide has no JPEG decoder for the static splash image),
  and its dissection HUD/labels match the device-style triple. The full
  animation features (quantum-number reveal, element/dissection intros,
  idle auto-advance) remain PC/device-only.

Orbital-name fix: the (n=5, l=2, m=0) preset was mislabeled "5p_z3"/"5pz3"
in `micropython/cloud_common.py` and `src/physics/orbital_library.h` (a d orbital,
not p) -- corrected to `5d_z2`/`5dz2` in both, which fixes the PC and web
ports too since they read the shared `ORBITAL_PRESETS` list.

## Constants and tuning reference

All viewer behavior is driven by module-level constants, most of them in
`viewer_common.py` (shared by both viewers), with the rest split between
`orbital_view_pc.py` (hydrogen viewer) and `atom_view_pc.py` (multi-electron
viewer). The code keeps only a one-line comment on each constant; the full
rationale behind the non-obvious values lives here, so the tuning decisions
stay documented without turning the source into prose.

### Display geometry (`viewer_common.py`, except `N_POINTS`)

| Constant | Value | Meaning |
| --- | --- | --- |
| `WIDTH` / `HEIGHT` | 480 / 480 | Logical render resolution (2× the device's 240×240 panel) |
| `CENTER` | `WIDTH // 2` | Screen center |
| `DISPLAY_SCALE` | 2 | The tkinter window is `WIDTH*DISPLAY_SCALE` square; all math stays at `WIDTH`/`HEIGHT` |
| `N_POINTS` (`orbital_view_pc.py`) | 5000 | Sampled points per preset -- 3000 on the device; the PC keeps a bit more for its 2×-resolution buffer without costing the 30fps target |
| `FRAME_DELAY_MS` | 5 | tkinter `.after()` delay -- a small pacing idle, not the throttle (the numpy render is) |

### Camera motion (`viewer_common.py`)

| Constant | Value | Meaning |
| --- | --- | --- |
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
| --- | --- | --- |
| `INTRO_START_SCALE_FACTOR` | 12.0 | Startup fly-over starts at 12× base scale |
| `INTRO_FRAMES` | 70 | Startup fly-over duration |
| `SWITCH_START_SCALE_FACTOR` | 10.0 | Orbital/Z-switch fly-over starts at 10× base scale |
| `SWITCH_TRANSITION_FRAMES` | 18 | Orbital/Z-switch fly-over duration |

### Random zoom excursions (`viewer_common.py`)

| Constant | Value | Meaning |
| --- | --- | --- |
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
| --- | --- | --- |
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
| --- | --- | --- |
| `PROTON_SIZE` | 4 | Nucleus marker size (px) |
| `PROTON_COLOR` | `(255, 0, 0)` | Nucleus marker color |

14px, not the device's 7: the PC buffer is 480×480 = 2× the 240 panel, so 2×
the panel px gives the same relative on-screen size. Matches today's device
change (3 → 7, "proton not visible enough, give him a bigger radius"). The
nucleus is drawn AFTER the cloud in `render_frame()` — fully opaque on top,
so a point landing on the same pixel can't dim/hide it (the device now
redraws the proton on top every frame for the same reason).

### Electron point rendering (`viewer_common.py`)

| Constant | Value | Meaning |
| --- | --- | --- |
| `ELECTRON_ALPHA` | 0.92 | Per-point blend fraction toward the point's own color |
| `ELECTRON_SIZE` | 2 | Electron point size (px per side, square block) |
| `ENABLE_PERSISTENCE` | True | Fade previous frame instead of clearing (PC-only) |
| `PERSISTENCE_DECAY` | 150 | /256 kept per frame (~0.59) |

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

`ELECTRON_ALPHA`/`PERSISTENCE_DECAY` were raised together (0.8→0.92,
100→150) to match the device's change (src/render/camera.h's
`kElectronAlphaQ8`/`kPersistenceKeepQ8`): during rotation a point rarely
lands on the exact same pixel two frames running, so it gets essentially one
blend toward full brightness before the persistence fade starts pulling that
pixel back down — the cloud reads visibly dimmer in motion than in a static
frame. A stronger alpha makes each hit closer to full brightness, and slower
decay keeps the glow alive between re-hits; the two are tuned together.
`PERSISTENCE_DECAY` was then pulled back 150→120 ("persistence is a bit too
much", 2026-08-17): the numpy fast path restored ~30 fps, so the same decay
value now spans roughly half as many wall-clock seconds and 150 read as too
long a trail.

`ELECTRON_SIZE` draws each point as a square block of that many pixels per
side (each block pixel blended at `ELECTRON_ALPHA`, so overlap still
converges toward full brightness). `1` = the old single-pixel dot. `2`
(double size) is the default because the PC buffer is 480×480 = 2× the
device's 240×240 panel: a 2×2 block here is exactly one device pixel, so the
PC preview shows electrons at the same apparent size as the panel instead of
half-size dots. Both `render_frame()` and the atom dissection view draw
through the shared `blend_electron()` (which has unrolled fast paths for the
common 1×1 and 2×2 sizes — it's called once per point per frame) so they
can't drift apart.

Persistence is a PC-only cosmetic: the device stays a hard clear+redraw (no
budget on-device). Fading instead of clearing makes points leave a trailing
glow as they tumble and softens the "buzz" turnover flicker (a skipped point
fades out instead of vanishing). The fade is applied via
`bytes.translate()` — one C-level lookup pass over the whole buffer,
effectively free at 480×480×3 bytes/frame; a per-byte Python loop would not
be. Lower `PERSISTENCE_DECAY` = shorter trails; 256 = never fades.

### Scale bar (`viewer_common.py`)

| Constant | Value | Meaning |
| --- | --- | --- |
| `SCALE_BAR_MARGIN_X` / `_Y` | 16 / 16 | Bottom-left margin (px) |
| `SCALE_BAR_MAX_PX` | 180 | Longest allowed bar |
| `SCALE_BAR_TICK_PX` | 8 | End-tick length |
| `SCALE_BAR_LINE_WIDTH` | 2 | Bar/tick line thickness (px) |
| `SCALE_BAR_FONT_SIZE` | 22 | Label font size (~2× the old default) |

Every dimension doubled (margins, tick height, line thickness) and the label
now drawn at a 2× font instead of PIL's tiny default — port of today's device
change (`src/render/overlay.cpp`: "la scaletta risulta illegibile, raddoppia le sue
dimensioni font compresa"). The "nice" round lengths and the length-picking
rule live in `micropython/cloud_common.py` (`SCALE_BAR_CANDIDATES` /
`pick_scale_bar_length()`), shared with the device renderer
(`micropython/orbital_view.py`), so a scale bar reads the same physical
length on both renderers at the same zoom. What's left in the PC code is
PIL-specific geometry and the draw calls. The bar is recomputed from the
frame's live pixels-per-unit every frame, so it tracks the camera's
zoom-breathing/excursion dives rather than only being accurate at rest
scale.

### HUD positions (`viewer_common.py`) and debug switches (`orbital_view_pc.py`)

| Constant | Value | Meaning |
| --- | --- | --- |
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
| --- | --- | --- |
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
| --- | --- | --- |
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

The clip is applied in camera space every frame, but `AtomViewApp._dissect_tumble()`
only advances roll during the whole sequence, never yaw/tilt: since
`rotate_yaw_tilt_roll()` computes the clipped depth `rz` from yaw and tilt
only (roll, a rotation about the view axis, never changes it — see that
function's docstring), freezing yaw/tilt keeps *exactly* the same half of
the cloud excluded for the whole sequence, camera and cloud spinning
together as one rigid unit, instead of the clip sweeping through fresh
material as the object tumbled underneath a camera-fixed cut plane (the
original behavior, confirmed unwanted). `DISSECT_FRAME_DELAY_S` paces every
leg of the sequence to the same rotation speed normal viewing uses: unlike
the fly-over transitions (no delay, run as fast as the CPU renders), the
sequence needs a real-time `DISSECT_HOLD_SECONDS` pause to be legible, so
all legs pace themselves the same way for a consistent rotation speed
throughout. The dissection view also deliberately has no persistence — with
the clip plane re-applied fresh to a continuously rotating cloud, a
trailing glow would smear the cut edge instead of reading as motion.

Note that clipping the point cloud alone does not visually shrink it to a
hemisphere: with an orthographic projection, the on-screen silhouette of a
solid ball sliced through its center is identical to the full ball's (every
screen (x, y) position inside the sphere's outline has points at some depth
in the remaining half, front or back), so the cut only reads as the cloud
thinning out, not narrowing — confirmed empirically (a uniform test cloud's
2D bounding box is unchanged after dropping the near half; only its point
density roughly halves, uniformly across the disc, not just near the rim).
The `rz > clip_z` drop itself is correct and verified (e.g. carbon's cloud:
almost exactly half its 10000 points survive `clip_z = DISSECT_CLIP_OPEN`).
The reference equator ring that used to accompany the cut was removed
entirely from the visualization (see below), so the cut now reads only as
the cloud thinning out.

Both this view and the normal (non-dissecting) one draw a plain gray
bounding-circle outline (`draw_bounding_circle()`, the neutral
`BOUNDING_SPHERE_COLOR` — deliberately NOT shell-colored) tracking the
reference sphere, so the eye has a recognizable sphere silhouette to track
even when the active subshell's dimming (`DISSECT_SHADE_GRAY`) makes the
actual points hard to see. The normal view layers it through
`draw_orbit_marker()` (which adds its own rotating spoke/marker text); the
dissection view calls `draw_bounding_circle()` directly, skipping the
spoke/text.

### Keyboard IMU (`keyboard_imu.py`)

| Constant | Value | Meaning |
| --- | --- | --- |
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
| --- | --- | --- |
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
