# SLICE.md — Orbital plane-slice view: implementation plan

This is the design + implementation plan for adding a **plane-slice heatmap of the current
hydrogen orbital** to the ESP32 orbital viewer, triggered by the **same gesture as the atom
shell dissection** (Right tilt-hold, `TiltDirection::kRight` confirmed). It is written and
reviewed before any implementation starts; code follows only after this document is approved.

Reference: the 2D x–z plane |ψ|² heatmaps of
[ssebastianmag/hydrogen-wavefunctions](https://github.com/ssebastianmag/hydrogen-wavefunctions)
(`hwf_plots.py` / `hydrogen_wavefunction.py`) — reviewed in-session. We adopt the *slice*
concept, not its rendering stack.

---

## 1. Goal

- A Right tilt-hold in the orbital view (currently a logged no-op, see `orbital_view.h`'s
  header comment) starts a self-contained, auto-playing **slice sequence** for the current
  preset: a brief intro card, then a full-screen 2D density heatmap of |ψ|² through a
  sequential colormap (rocket-like, D1) on a fixed plane through the orbital (static, D2),
  then an automatic fade-out back to the 3D cloud.
- The interaction contract mirrors `atom_view.cpp`'s `runDissectionSequence()` exactly:
  * one gesture starts the whole sequence (no per-step gestures);
  * any tilt movement mid-sequence aborts it and returns to the full 3D view;
  * the sequence always ends back on the untouched 3D view (camera not modified).
- Same gesture, same feel as the atom dissection — the user asked for exactly that parity.

## 2. Why this is worth doing (from the review)

- We currently have only the 3D point cloud; the reference repo is *only* slices. A slice is
  a genuinely new visual mode and a much better "textbook picture" of an orbital.
- Our real-orbital convention (`psiReal()`, `orbitals.h`) gives the slice **actual lobe
  structure** (for m ≠ 0 the density varies with azimuth, so e.g. 2px shows a dumbbell while
  the reference's complex Y_l^m makes |ψ|² azimuthally uniform and lobe-less). The slice
  renders density only (nodes as dark regions) -- the phase sign stays in the 3D cloud.
- Everything needed already exists on-device: `psiReal` primitives, per-preset
  `radialCoeff`/`legendreCoeff` (`OrbitalResampleState`), `orbitalLevelToColor565()` for
  color, and the dissection gesture contract to copy.

## 3. Physics of the slice (the math, settled before code)

Real orbital (same convention as `orbitals.h::psiReal`):

    psi(r, th, ph) = R(r) * P(th) * A(ph),   A(ph) = cos(m*ph)  for m >= 0
                                                 A(ph) = sin(|m|*ph) for m < 0

The slice plane contains the z axis; its +x half sits at world azimuth ph0. An in-plane cell
at coordinates (x, z) maps to world (x*cos(ph0), x*sin(ph0), z), so:

    r       = sqrt(x^2 + z^2)          -- same for both halves
    th      = acos(z / r)              -- same for both halves
    world ph = ph0        for x > 0
               ph0 + pi   for x < 0

Key identity (both cos and sin cases):  A(ph0 + pi) = (-1)^|m| * A(ph0). Therefore

    psi(x, z) = base(r, th) * A(m, ph0)            for x > 0,   base = R(r)*P(th)
                base(r, th) * A(m, ph0) * (-1)^|m|  for x < 0
    |psi|^2   = base^2 * A(m, ph0)^2                -- identical on both halves

**D2 approved: the slice plane is STATIC** (reference-repo style still, animated only by a
fade-in/out). With a fixed plane there is no per-frame trig at all: the plane azimuth ph0 is a
per-preset constant chosen so the slice shows LOBES, not a node —

    ph0 = 0          for m >= 0   (cos-type: the x–z plane is a lobe plane; 2px, 3dxz, 3dx2-y2, …)
    ph0 = pi/(2|m|)  for m < 0    (sin-type: the x–z plane is a NODAL plane — psi = R*P*sin(|m|ph)
                                   is zero there, e.g. 2py, 3dyz, 3dxy; the lobe plane sits at azimuth
                                   pi/(2|m|), where sin(|m|*ph0) = ±1)

The per-cell value is then computed ONCE at build time, folding the azimuthal factor in:

    cell_value(x, z) = R(r)*P(th) * A(m, ph0) * (x < 0 ? (-1)^|m| : 1)

and the 2D pattern is fully static — the reference repo's exact situation, except that our
real orbitals make the lobe structure visible (m ≠ 0 density varies across the plane; the
slice renders density only, sign is not drawn -- see D1).

Sanity checks to validate in Python before firmware (see §9): the 2px slice (ph0 = 0) is a
dumbbell along ±x (two bright lobes); the 2py slice (ph0 = pi/2) is the y-dumbbell (the x–z
slice of 2py is all zero — the reason ph0 is per-sign); the 3dxy slice (ph0 = pi/4) shows
the same-phase 45°/225° lobe pair; 3dz2 (m = 0) is static and axially patterned; the s
orbitals show a single bright core (2s/3s with their radial node as a dark ring). The
harness additionally validates the SIGN structure of the underlying ψ (dumbbell split,
(-1)^|m| half-flip) against its own computation -- physics checks that hold even though the
rendered heatmap is density-only.

## 4. Visual design decisions — flagged for approval

**D1 — Slice coloring. REVISED after implementation (user feedback, then approved):
sequential density heatmap.** Brightness = density-normalized |ψ|² (level, see §5), color =
a 16-stop rocket-like sequential ramp (`kSliceColormapStops`, near-black → deep purple →
magenta → coral → pale yellow-cream), density-only — the phase sign is NOT drawn. The
phase-colored orange/blue version was built first and rejected on hardware/preview: with the
same hue pair as the 3D cloud it read as "the 3D view, flattened" rather than as a heatmap.
The sequential ramp (the reference repo's own visual language) is what makes the slice read
as a density map: bright cores, dark node lines/rings, smooth gradients.

**D2 — Plane animation. RESOLVED by user: static slice.** The plane stays at the lobe-plane
azimuth ph0 of §3 (0 for m >= 0, pi/(2|m|) for m < 0). The only motion is a fade-in over
kSliceIntroFrames and a fade-out over kSliceFadeOutFrames; the heatmap itself is a still,
exactly like the reference repo's plots. (A rotating-plane "node sweep" was the alternative,
rejected: it adds an animation but fades the whole image through nodal planes instead of
rotating the pattern — the pattern is invariant under plane rotation, see §3.)

**D3 — Hold/auto-return. RESOLVED by user: 12 s.** The slice displays for kSliceHoldUs
(12 s), then auto-fades back to the 3D view, mirroring the dissection's auto-return. Any
tilt movement aborts earlier. (Alternative — stay until Left-hold — rejected: it would
compete with the menu-return gesture and break dissection parity.)

**D4 — Intro card. RESOLVED by user: "Sezione".** Brief (~900 ms) card in the orbital intro
aesthetic: dim equation backdrop (`drawEquationBackdrop`) + "Sezione" (kFontHuge, accent)
+ the preset's n/l/m line (kFontLarge, white), mirroring `scrollOrbitalIntro()`'s look (and
atom dissection's "Configurazione elettronica" card role).

## 5. Rendering architecture

New module `src/views/orbital_slice.h` / `src/views/orbital_slice.cpp` (view-level: uses
physics primitives + Display; the sequence orchestration stays in `orbital_view.cpp` like
`runDissectionSequence()` stays in `atom_view.cpp`).

```cpp
// Grid: full panel resolution, 240x240 cells at 1 px each -- smooth, imshow-style.
inline constexpr int kSliceGridSize = Display::kDisplayWidth;

struct SliceTable {
    int n, ell, m;
    orb_real_t planeAzimuth;         // lobe-plane ph0: 0 for m >= 0, pi/(2|m|) for m < 0
    orb_real_t extentBohr;           // grid half-extent, kSliceFramingFactor * rRef (bohr)
    orb_real_t extentPm;             // same, in pm -- feeds drawScaleBar()
    // Density-normalized brightness 0..255 (no sign channel -- density-only render):
    // 255*min(1, |psi|^2/v99)^gamma, v99 = the grid's own 99.9th percentile, gamma computed
    // per orbital by an auto-exposure pass (see below).
    uint8_t level[kSliceGridSize * kSliceGridSize];
};

// 16-stop rocket-like sequential ramp (near-black -> purple -> magenta -> coral -> cream),
// per-channel lerp at render time. Density-only by design (see D1).
inline constexpr SliceColorStop kSliceColormapStops[] = { ... }; // in orbital_slice.h

// One-time build: sample base = R(r)*P(th) at cell centers, fold the azimuthal sign flip,
// density-normalize |psi|^2 against the grid's own 99.9th percentile (v99 via nth_element).
void buildSliceTable(int n, int ell, int m, const orb_real_t *radialCoeff,
                     const orb_real_t *legendreCoeff, orb_real_t rRef, SliceTable *out);

// Per-frame: fade in [0,1]. One pixel per cell (no blocks, no interpolation), colored via
// kSliceColormapStops. No per-frame trig.
void renderSliceFrame(uint16_t *frameBuf, const SliceTable &t, orb_real_t fade);
```

- Cells sample at their **centers** (r = 0 only for the exact center cell; R(0) = 0 for
  ell > 0 anyway — no division hazard; z/r clamped to [-1, 1]).
- Static PSRAM scratch (`EXT_RAM_BSS_ATTR`, following `atom_view.cpp`/`orbital_view.cpp`
  precedent): `sliceMag[57600] float` + `sliceOrder[57600] int` for the one-time v99 pass
  (~460 KB transient), `SliceTable` itself (~58 KB). Total ≈ 520 KB PSRAM transient,
  trivial against 8 MB.
- **Full-resolution grid (post-implementation change, user-approved):** 240x240 cells at
  1 px each (was 120x120 at 2x2 blocks) — the blocky 2x2 look read as a low-res copy of the
  cloud; one-pixel cells give the smooth imshow-style gradients the reference repo has.
- The heatmap is a **plain overwrite** (no blend, no fade-persistence): each frame redraws
  every cell at full brightness scaled by `fade`. No trails needed for a solid image.
- **Brightness mapping (post-review change to the original plan, user-approved):** density
  normalization, NOT rank. level = 255·min(1, |ψ|²/v99)^gamma, with v99 = the grid's own
  99.9th percentile of |ψ|² (self-normalizing per orbital, same convention as the reference
  repo's vmax). Rationale (confirmed numerically during review): rank-equalizing a uniform
  grid lights half the screen to ~83% brightness by construction (median level 212 for every
  orbital, 0% dark cells) — the 3D cloud gets away with rank because its density-weighted
  point samples leave empty screen space black; a grid has no empty space. Density
  normalization preserves the real falloff (bright lobe cores, near-black tails) and is what
  produces the reference repo's crisp look. v99 is found with std::nth_element on the index
  array of |ψ| (not |ψ|² directly — squaring is monotonic over non-negative values, so the
  percentile-of-|ψ| squared equals the percentile-of-|ψ|²) — no full sort, no extra scratch.
- **Auto-exposure (post-hardware-review fix, user-reported oversaturation).** A single fixed
  gamma for every orbital was wrong: on hardware, orbitals whose density already fills much of
  the visible footprint (e.g. higher-|m| states) looked oversaturated/washed out under the
  same gamma lift that a tightly-peaked orbital (e.g. an s state) needs to reveal its dim
  tails. The reference repo hits the same issue and its `plot_hydrogen_wavefunction_xz()`
  fixes it with an `exposure` parameter, hand-picked per rendered image (`gamma =
  max(0.1, 1/(1+exposure))`, `exposure=0` — i.e. plain linear, `gamma=1` — for orbitals that
  already fill the frame, up to `1.5` for tightly-peaked ones; see its `main.py`). We can't
  hand-pick per preset the same way: our orbitals are real (`psiReal()`), which gives genuine
  lobe structure even for m ≠ 0 (§2's phi-dependence note), unlike the reference's
  azimuthally-uniform complex harmonics — so its per-(n,l,m) exposure choices don't transfer.
  Instead `buildSliceTable()` computes the equivalent automatically per orbital: after v99,
  one counting pass over the grid finds `brightFraction` = (cells with density ≥
  `kSliceBrightFrac`·v99) / (cells with density ≥ `kSliceVisibleFrac`·v99) — what fraction of
  the *visible* cloud (density above a small v99-relative floor, i.e. excluding background) is
  already near-max. A concentrated orbital has a low brightFraction (small hot core against a
  dim visible tail) → more exposure lift; a broad orbital has a high brightFraction (density
  already near v99 across much of its footprint) → little to none, avoiding the saturation the
  flat gamma caused. `exposure = kSliceExposureScale·(1 − brightFraction)`, `gamma =
  max(kSliceMinGamma, 1/(1+exposure))` — the reference's own formula, applied per orbital
  instead of by hand. Constants in `visual_constants.h`; `pc/orbital_slice_pc.py` mirrors the
  same computation for parity.
- No proton marker, no bounding circle, no camera motion during the slice (it is a 2D
  presentation; the camera is left untouched so the 3D view resumes seamlessly).

## 6. Code changes, file by file

1. **NEW `src/views/orbital_slice.h` + `.cpp`** — §5's `SliceTable`/`buildSliceTable()`/
   `renderSliceFrame()`. Also a tiny `drawSliceOverlay()` (preset title top-left, orbital
   numbers bottom-right, scale bar bottom-left) or that stays inline in the sequence.
   **Post-implementation addition (user-requested):** a small "densita di probabilita" legend
   right under the title, kFontSmall/kScaleBarColor -- once D1 dropped phase coloring for a
   density-only heatmap, the color ramp no longer speaks for itself the way the phase-colored
   3D cloud does, so the plot names what it's showing. Same legend line added to
   `orbital_slice_test.cpp`'s `SLICE_TEST` harness for parity.
2. **EDIT `src/views/orbital_view.h`** — add `orb_real_t rRef;` to `OrbitalPresetState`
   (currently dropped by `load()`; the slice needs it for framing). Update the header's
   tilt-gesture comment: Right-hold now = slice, matching atom dissection.
3. **EDIT `src/views/orbital_view.cpp`** —
   - `load()`: keep `scale.rRef` in the new member.
   - Local `runSliceSequence(display, preset, tilt)` (anonymous namespace, mirroring
     `runDissectionSequence`'s structure): intro card → `buildSliceTable()` (log build ms,
     like `loadMs`) → fade-in over kSliceIntroFrames → hold for kSliceHoldUs (poll tilt
     every frame, any non-idle phase aborts; draw tilt arrow as feedback) → fade-out over
     kSliceFadeOutFrames → return.
   - Right-hold branch: replace the logged no-op with the sequence call, then reset
     `zoomAngle`/`zoomExcursionCountdown`/`stats` exactly like atom_view's dissection does.
   - Idle auto-advance: mirror atom_view's idle-dissection — an `idleSlicedThisPreset` flag
     plus a 50% coin flip to slice the current preset instead of jumping (once per preset).
4. **EDIT `src/physics/orbital_presets.h` + `.cpp`** — promote the file-local
   `levelFromRankFraction()` and `kOrbitalColorMinLevel`/`kOrbitalLevelGamma` to the header
   (as `orbitalLevelFromRankFraction()` + inline constants). NOTE (post-review change): the
   slice does NOT use this curve — it uses the density normalization described in §5. The
   promotion stays (the 3D cloud still uses it) and keeps the option open, but the
   "byte-identical curve" requirement from the original plan is superseded for the slice.
5. **EDIT `src/config/visual_constants.h`** — new "Orbital slice view" section:
   `kSliceGridSize = 240` (full panel resolution, one cell per pixel), `kSliceFramingFactor =
   1.2` (half-extent = factor·rRef), `kSliceHoldUs = 12 s`, `kSliceIntroHoldMs = 900`,
   `kSliceIntroFrames = 30` (fade-in), `kSliceFadeOutFrames = 20`,
   `kSliceDensityPercentile = 0.999`, and the auto-exposure group `kSliceVisibleFrac = 0.01`,
   `kSliceBrightFrac = 0.5`, `kSliceExposureScale = 1.5`, `kSliceMinGamma = 0.10` (see §5's
   auto-exposure note). No angular-speed constant: the plane is static (D2);
   the lobe-plane azimuth ph0 is a per-preset value computed in `buildSliceTable()` (§3), not
   a tunable.
6. **NEW `src/debug/orbital_slice_test.h` + `.cpp`** (recommended) — a `SLICE_TEST` toggle
   in `main.cpp` that boots straight into slice sequences for a few presets (2px, 2pz,
   3dx2−y2, 4fz3, 1s), ~3 s each, no IMU needed — the quick way to eyeball patterns and
   tune constants on hardware without the full chooser flow.
7. **EDIT `main.cpp`** — add the `SLICE_TEST` define (commented out, like the others).
8. **NEW `pc/orbital_slice_pc.py`** (recommended) — standalone tkinter+PIL preview of the
   same animation reusing `micropython/` math, for iterating visuals without flashing *and*
   as the §9 math-validation harness.

## 7. Performance and memory budget

- One-time build: 57 600 cells × (R eval: polynomial + r^ell + exp; P eval: polynomial +
  sin^|m|; one folded azimuthal multiply) ≈ ~40–120 ms once (after the intro card; no frame
  deadline). No per-frame trig at all (static plane).
- Steady state: 57 600 × (clamp, 16-stop lerp, 565 pack, 1 pixel write) — comfortably inside
  the existing 16 ms frame budget at 62.5 FPS; the slice loop is vTaskDelay-paced anyway.
- Memory: ≈ 520 KB transient static PSRAM (see §5); `OrbitalPresetState` already lives in
  PSRAM.
- The 3D cloud machinery (`renderScene`, persistence, buzz) is untouched; the slice uses its
  own small loop, so there is zero regression risk to the measured 62.5 FPS path.

## 8. Out of scope (explicitly deferred)

- The 3D-cloud colormap LUT rework — still deferred; the slice-scoped sequential ramp
  (kSliceColormapStops) is implemented and does not touch the cloud's phase-colored
  brightness mapping.
- A 3D cutaway (slice plane embedded in the tumbling 3D view) — a possible future mode,
  deliberately not part of this change (different rasterizer, muddiness risk).
- Complex (non-real) orbitals — our library is real orbitals by design.
- Slice stills in `screenshot_batch.cpp` — can be added later via `renderSliceFrame(fade=1)`;
  noted, not implemented now.
- Full `pc/orbital_view_pc.py`/web parity — only the small standalone preview script (§6.8).

## 9. Validation plan

1. **Math**: in `pc/orbital_slice_pc.py`, assert the §3 sanity checks: 2px (ph0 = 0) shows a
   dumbbell along ±x, orange right / blue left; 2py (ph0 = pi/2) the y-dumbbell with the same
   split; the ph0 = 0 slice of 2py is all zero (why ph0 is per-sign); 3dxy (ph0 = pi/4) shows
   a same-phase lobe pair (both halves +1); 3dz2 and the s orbitals are static; 2s shows the
   radial node as a dark ring. Runs before any firmware flash.
2. **Build**: user runs `pio run` (I do not build/flash per project instructions); fix any
   compile errors from the report.
3. **Device (user)**: `SLICE_TEST` build → check intro card, per-preset patterns, fade
   in/out, abort-on-movement; then normal boot → chooser → orbital view → Right-hold →
   slice → auto-return; Right-hold again; idle coin-flip after 60 s idle.
4. **Constants**: any color/pacing tuning lands only in `config/visual_constants.h` /
   `orbital_slice.h` per the adjustable-by-eye convention.

## 10. Decisions (user-approved) and remaining question

Resolved: D1 sequential density heatmap (rocket-like colormap, density-only — revised from
phase-colored after implementation feedback) · D2 static slice (no rotating plane) · D3 12 s
hold · D4 "Sezione" intro card.

Post-review changes (user-approved): (1) density normalization (percentile-clipped |ψ|² +
gamma) instead of the 3D cloud's rank curve — see §5's contrast note; (2) full-resolution
240x240 grid instead of 2x2 blocks; (3) sequential rocket-like colormap instead of the
orange/blue phase pair (D1). Implementation complete.

Post-hardware-review fix (user-reported oversaturation): the v99 percentile was taken of |ψ|
but used as if it were already |ψ|² (missing a square — v99 must be squared before dividing
|ψ|² by it), and a single fixed gamma (0.5) was applied to every orbital regardless of shape.
Fixed: v99 is now correctly squared, and gamma is computed per orbital via an auto-exposure
pass mirroring the reference repo's own hand-tuned `exposure` parameter (see §5's auto-exposure
note). Remaining: hardware validation (`pio run` → SLICE_TEST → full flow), then
`kSliceExposureScale`/`kSliceBrightFrac`/`kSliceVisibleFrac` tuning by eye if needed.

---

*Plan written before implementation; revised in place as the design evolved through review
and hardware feedback. Current state matches the committed code on the `orbital-slice-view`
branch.*
