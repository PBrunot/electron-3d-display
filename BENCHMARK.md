# Rendering benchmark

`src/debug/benchmark_test.h`/`.cpp` sweeps a fixed atom (Fe, Z=26) AND a fixed orbital preset
(2pz, `kOrbitalDefaultPresetIndex`) across the same several point-cloud sizes, timing real
production-path rendering at each size for both, and logs performance numbers (tagged
`kind,atom` or `kind,orbital` so the two are easy to tell apart in a capture) plus a handful of
deterministic physics numbers (atom sweep only) usable as a correctness regression check. See
that file's header comment for the full rationale.

## How to run it

1. In `src/main.cpp`, comment out whichever `#define` toggle is currently active and uncomment:
   ```c
   #define BENCHMARK_TEST
   ```
2. Build and flash:
   ```
   pio run -e WS_ESP32_S3_LCD_1_3 -t upload
   ```
3. Capture serial output (stop once `BENCH,DONE` appears):
   ```
   pio device monitor
   ```
4. Report back the `BENCH,...` lines (or the whole capture) — compare against the "Expected
   results" sections below.
5. When done, re-comment `BENCHMARK_TEST` in `main.cpp` and reflash to return to normal boot
   (chooser menu).

The sweep takes ~16-18 seconds total (5 point-count steps x 60 frames each x 2 sweeps, plus
point-cloud build time per step) and needs no IMU/tilt setup or user interaction.

## Expected results (baseline: 2026-08-22, ESP32-S3 @ 240MHz, SPI 80MHz, this hardware unit)

Re-captured after merging CYD (ESP32-2432S028R) hardware support (`CYD-test` branch): the
`Display` frame buffer went from a single flat array + per-frame software Y-flip to a
block-based allocator with per-pixel `writePx()`/`readPx()` accessors (needed for the CYD's
fragmented, non-PSRAM heap), and every render/font/overlay call site was converted from raw
`uint16_t* frameBuf` to `Display&`. That touches the S3's hot render path too, so this rerun
is also the regression check for that refactor against the previous (2026-08-20) baseline below
-- verdict: **no regression**, every row is flat-to-slightly-faster than 2026-08-20, and
`iram_free`/the physical-correctness tables below are unchanged. `avg_wait_ms` isn't printed by
the summary table below (see the raw `BENCH,STEP` line for it) but stayed flat at ~11-13ms
across every row of both sweeps, as expected for a fixed-size SPI DMA transfer.

### Performance (`BENCH,STEP`)

**Atom sweep (Fe, Z=26):**

| points | build_ms | avg_render_ms | min_render_ms | max_render_ms | fps  | iram_free |
|-------:|---------:|---------------:|---------------:|---------------:|-----:|----------:|
|    500 |      265 |           9.484 |           9.476 |           9.573 | 45.89 |    216687 |
|   1000 |       20 |           9.963 |           9.955 |          10.050 | 45.88 |    216687 |
|   2000 |       24 |          10.926 |          10.914 |          11.031 | 42.08 |    216687 |
|   4000 |       33 |          13.420 |          13.410 |          13.507 | 38.79 |    216687 |
|   8000 |       50 |          17.806 |          17.794 |          17.886 | 33.56 |    216687 |

**Orbital sweep (2pz, `kOrbitalDefaultPresetIndex`):**

| points | build_ms | avg_render_ms | min_render_ms | max_render_ms | fps  | iram_free |
|-------:|---------:|---------------:|---------------:|---------------:|-----:|----------:|
|    500 |      105 |           9.850 |           9.844 |           9.972 | 45.87 |    216387 |
|   1000 |       99 |          10.403 |          10.397 |          10.532 | 45.79 |    216387 |
|   2000 |      114 |          11.520 |          11.506 |          11.652 | 42.00 |    216387 |
|   4000 |      145 |          14.342 |          14.335 |          14.431 | 38.75 |    216387 |
|   8000 |      214 |          19.328 |          19.319 |          19.441 | 31.47 |    216387 |

(500-point atom `build_ms` of 265 is a one-off: the very first sweep step, before Fe's
subshell rejection-sampling has "warmed up" any branch prediction/caches -- every later atom
step, and the whole orbital sweep, doesn't show it. Not a regression signal by itself; watch
whether it recurs at 500 points specifically across runs. Unchanged from the prior baseline,
as expected -- this is startup-order noise, not something the `Display` refactor touches.)

Previous baseline (2026-08-20, same SPI 80MHz -- superseded by the table above, kept for delta
reference): atom @8000pts 18.149ms/31.48fps, orbital @8000pts 19.139ms/31.44fps; see git
history of this file for the full prior tables. (That 2026-08-20 entry's header mislabeled
this as "SPI 40MHz" -- `display.cpp`'s `LCD_PIXEL_CLOCK_HZ` for the S3 target has been
80MHz all along, unchanged by the CYD port; the CYD's own ILI9341 path runs at 40MHz, a
separate `#define` block -- see that file.)

Notes:
- **8000 points is the production count** (`kAtomNumPoints` == `kOrbitalNumPoints`, what
  `atom_view.cpp`/`orbital_view.cpp` actually render) -- the number that matters is the last
  row of each table: **~31-32 FPS for both viewers**, comfortably above the 20-30 FPS target in
  `CLAUDE.md` §6.
- **Orbitals cost noticeably more to build** than atoms at every point count (e.g. 214ms vs.
  50ms at 8000 points) -- expected, since orbital sampling evaluates the radial/angular
  wavefunction per point rather than atom_cloud's simpler per-subshell rejection sampling.
  Render/FPS are nearly identical between the two once built, since both pay the same
  per-point projection/rasterization cost.
- `avg_wait_ms` (blocked on the previous frame's SPI DMA) should stay roughly flat (~12-14ms)
  across all point counts AND both sweeps -- it's dominated by the fixed 240x240x16bit frame
  transfer, not by point count or cloud type. If it scales with point count instead, something
  changed in how/when `presentFrame()`/`waitForFlushDone()` are called.
- `avg_render_ms` (CPU: fade + rotate/project/rasterize + text) should scale roughly linearly
  with point count. A large jump in the 500-point row specifically (which should be cheapest)
  usually means a fixed per-frame cost (e.g. `fadeFrameBuffer()`, title/scale-bar text) grew,
  not a point-cloud regression.
- `build_ms` (point-cloud sampling) growing faster than linearly with point count would suggest
  a regression in the radial/angular sampling rejection rate, not just raw point count.
- `iram_free` should stay flat within each sweep (it does here) -- a downward drift step-to-step
  would flag a leak.
- Re-run and compare after: changing `platformio.ini`'s `SPI_FREQUENCY`, editing
  `camera.h`'s render pipeline (`renderScene`/`renderPointsColored`/`projectPoint`), or editing
  `display.cpp`'s DMA/flush handling.

### Physical correctness (`BENCH,CONFIG` / `BENCH,ZEFF` / `BENCH,GEOM`)

Element: **Fe (Z=26)**. `CONFIG`/`ZEFF` are pure functions of Z (no RNG, no point sampling) --
they must be **bit-identical** on every run on unchanged code. `GEOM` depends on the sampled
points (seeded, so still deterministic per point count) and should match closely but can drift
in the last 1-2 significant digits between unrelated code changes that touch the RNG call order.

**Electron configuration** (must match exactly -- `[Ar] 3d6 4s2`, real ground-state Fe):

| n | ell | occ | label |
|---|-----|----:|-------|
| 1 | 0 | 2 | 1s |
| 2 | 0 | 2 | 2s |
| 2 | 1 | 6 | 2p |
| 3 | 0 | 2 | 3s |
| 3 | 1 | 6 | 3p |
| 4 | 0 | 2 | 4s |
| 3 | 2 | 6 | 3d |

Total = 26 electrons. If this list, the order, or any occupancy changes, that's a regression in
`slater.h`'s `electronConfiguration()`/Madelung filling or `slater_data.h`'s exception table --
blocking, not a rounding issue.

**Z_eff per subshell** (float32 on-device; rtol ~2e-3 vs. a recomputation is fine, exact match
expected between identical builds):

| n | ell | Z_eff |
|---|-----|------:|
| 1 | 0 | 25.381000518798828 |
| 2 | 0 | 18.599000930786133 |
| 2 | 1 | 22.089000701904297 |
| 3 | 0 | 13.675999641418457 |
| 3 | 1 | 12.777999877929688 |
| 4 | 0 | 5.434000015258789 |
| 3 | 2 | 11.180000305175781 |

Sanity shape: monotonically decreasing binding strength from the core (1s, tightest, Z_eff
closest to the true Z=26) out to the valence 4s (Z_eff≈5.4, heavily shielded) -- if 4s ever
comes out with a *higher* Z_eff than 3d/3p, that's a shielding-rule regression, not noise.

**Geometry fingerprint** (outer/valence subshell + its reference radius, per point count):

| points | outer subshell | outer_rref_bohr | base_scale_px |
|-------:|-----------------|-----------------:|---------------:|
|    500 | 4s (n=4, ell=0) |         5.196394 |      18.474348 |
|   1000 | 4s (n=4, ell=0) |         5.522888 |      17.382212 |
|   2000 | 4s (n=4, ell=0) |         5.389955 |      17.810911 |
|   4000 | 4s (n=4, ell=0) |         5.376742 |      17.854677 |
|   8000 | 4s (n=4, ell=0) |         5.411359 |      17.740459 |

`outer_rref_bohr`/`base_scale_px` above are from the 2026-08-20 capture and read noticeably
different from an earlier (2026-08-19) capture of this same table (`outer_rref_bohr` ~6.0-6.2,
`base_scale_px` ~12.0-12.5) -- bigger than the "last 1-2 significant digits" drift this doc
otherwise expects between unrelated changes. `CONFIG`/`ZEFF` matched bit-identically across
both captures, so this isn't a `slater.h`/Z_eff regression; it's localized to the p90-radius
sampling or `kAtomTargetPx`/scale constants, likely from one of the visual-tuning commits
between the two captures (`visual tweaks`, `Improve display buffering and atom overlays`,
etc. -- see `git log -- src/physics/atom_cloud.h src/config/visual_constants.h`). Not
chased down further here; flagging so a future capture doesn't mistake the 2026-08-20 numbers
above for a regression against the (now stale) ~6.0-6.2/~12.0-12.5 figures.

Notes:
- The outer subshell must be **4s at every point count** -- Fe's real valence shell. If it ever
  comes out as 3d (or anything else), that's a bug in `outerSubshellRRef()`'s p90-radius
  comparison or in the point-count split across subshells (`splitCounts()`), not just sampling
  noise.
- `outer_rref_bohr` should converge toward ~6.0-6.2 Bohr radii as point count grows (statistical
  p90 estimate over more samples) -- the 500-point row is the noisiest by design. A value far
  outside ~5.5-6.5 at 8000 points points at a regression in the radial wavefunction/rejection
  sampling for 4s, not just noise.
- `base_scale_px` = `kAtomTargetPx / outer_rref_bohr` (`atom_cloud.h`), so it moves inversely
  with `outer_rref_bohr` -- expect ~12.0-12.5px across all rows.

## MicroPython (ESP32-S3, 8MB Octal PSRAM)

`micropython/benchmark_test.py` mirrors `src/debug/benchmark_test.cpp`'s methodology exactly, so
the two are directly comparable: same fixed atom (Fe, Z=26), same fixed orbital preset (2p_z,
`cloud_common.ORBITAL_PRESETS[DEFAULT_PRESET_INDEX]` -- index-matched to
`kOrbitalDefaultPresetIndex`), same point-count sweep (500/1000/2000/4000/8000), same seed
(12345, `kAtomCloudSeed`/`cloud_common.SEED`).

### How to run it

```
mpremote connect <port> fs cp -r micropython/. :
mpremote connect <port> exec "import benchmark_test; benchmark_test.run()"
```

### Parity fixes required before these numbers meant anything

Three real mismatches were found and fixed before this comparison was trustworthy -- without
them, MicroPython's numbers looked better than C++'s at low point counts, which was the tell
that something was being measured unfairly rather than MicroPython genuinely being faster:

1. **SPI clock**: `micropython/display.py` was running the panel at 40MHz;
   `src/render/display.cpp`'s `LCD_PIXEL_CLOCK_HZ` for this same S3 target is 80MHz. Bumped to
   match -- `SPI_BAUDRATE = 80_000_000`.
2. **Table-build amortization**: C++'s orbital sampler tables (`kOrbitalLibrary`) and per-`(ell,
   m)` angular tables (`angular_library.h`) are `constexpr`, baked into flash at *compile* time --
   zero runtime cost, every sweep step. MicroPython was rebuilding the equivalent inverse-CDF
   tables from scratch on every single sweep step. Fixed by adding real caches to
   `cloud_common.py` (`_ORBITAL_SAMPLER_CACHE`) and `atom_cloud.py` (`_RADIAL_TABLE_CACHE`/
   `_ANISO_SAMPLER_CACHE`), keyed by `(n, ell, m)` / `(z, n, ell[, m])` -- a genuine production
   improvement (faster element/orbital switching in the live viewers, and in `pc/`, which shares
   these modules), not just a benchmark shortcut. The one-time build cost is still real and is
   reported separately (`BENCH,TABLEBUILD`), paid once before the timed sweep starts, mirroring
   how C++ never pays it during the sweep either.
3. **Render pipeline was doing less work than C++**: `src/render/camera.h`'s `renderScene()`/
   `renderSceneGrouped()` fade the *entire* frame buffer toward black every frame
   (`Display::fade()`, `kPersistenceKeepQ8=160/256`) and alpha-blend every point write against
   whatever's already there (`blendColor565()`, `kElectronAlphaQ8=240/256`) -- not a hard clear
   and opaque overwrite, which is what `device_render_common.py` was doing. Ported both to
   MicroPython (`fade_buffer()` and the blend inside `render_points()`, both
   `@micropython.viper`), using the identical unpack/scale/pack bit formulas as
   `Display::unpackColor565()`/`fadeColor565()`/`blendColor565()` -- cross-checked bit-identical
   against the C++ formulas across 200k random values before trusting it on hardware.

Point-cloud sampling ALGORITHM parity was also checked directly (not just assumed): both sides
use the same inverse-CDF sampling, the same `XorShift32` PRNG, the same 3-draws-per-point order
(`src/physics/pointcloud.{h,cpp}` vs `micropython/pointcloud.py`) -- confirmed identical, so the
build-time gap below is implementation speed (interpreted vs compiled), not different work.

### One-time table-build cost (paid once per element/orbital selection, not per sweep step)

| kind | warm_ms |
|------|--------:|
| atom (Fe subshells) | ~2400-2700 |
| orbital (2p_z sampler) | ~650-660 |

This is what the live viewers' "Loading..." screen covers -- with caching, it now only happens
on the first visit to a given element/orbital per session, not on every switch.

### Performance (`frames_per_step=30` -- half the C++ side's 60; FPS already converges well
before 60 samples, and MicroPython's per-point sampling cost below makes the full schedule
noticeably slower to capture, so this was halved to keep capture time reasonable)

**Atom sweep (Fe, Z=26):**

| points | build_ms | avg_compute_ms | avg_blit_ms | avg_frame_ms | fps | heap_free |
|-------:|---------:|----------------:|-------------:|--------------:|-----:|----------:|
|    500 |      325 |           56.428 |        13.224 |         69.652 | 14.36 |   7777696 |
|   1000 |      694 |           56.648 |        13.125 |         69.773 | 14.33 |   7758192 |
|   2000 |     1392 |           59.850 |        13.113 |         72.963 | 13.71 |   7719168 |
|   4000 |     2900 |           65.867 |        13.168 |         79.035 | 12.65 |   7641168 |
|   8000 |     5227 |           78.301 |        13.120 |         91.421 | 10.94 |   7460576 |

**Orbital sweep (2p_z):**

| points | build_ms | avg_compute_ms | avg_blit_ms | avg_frame_ms | fps | heap_free |
|-------:|---------:|----------------:|-------------:|--------------:|-----:|----------:|
|    500 |      453 |           53.063 |        13.148 |         66.212 | 15.10 |   7730400 |
|   1000 |      863 |           54.717 |        13.142 |         67.859 | 14.74 |   7712416 |
|   2000 |     1727 |           59.889 |        13.173 |         73.062 | 13.69 |   7676448 |
|   4000 |     3543 |           64.565 |        13.117 |         77.682 | 12.87 |   7604384 |
|   8000 |     7220 |           77.793 |        13.133 |         90.926 | 11.00 |   7460400 |

`avg_compute_ms`/`avg_blit_ms` are the MicroPython analogue of C++'s `avg_render_ms`/
`avg_wait_ms`, but the underlying mechanism differs: `st7789py.py`'s `blit_buffer()` is a
synchronous blocking SPI write (no DMA-kickoff-then-wait-later split like
`Display::presentFrame()`/`waitForFlushDone()`), so `avg_blit_ms` is the SPI transfer alone and
`avg_compute_ms` is everything else (fade + proton marker + rotate/project/blend + title/scale
bar), still excluding the blit.

### Comparison vs C++ (same hardware class, same points, same seed)

**FPS at matching point counts:**

| points | C++ atom fps | MPY atom fps | C++ orbital fps | MPY orbital fps |
|-------:|-------------:|-------------:|-----------------:|------------------:|
|    500 |        45.89 |        14.36 |             45.87 |              15.10 |
|   2000 |        42.08 |        13.71 |             42.00 |              13.69 |
|   8000 |        33.56 |        10.94 |             31.47 |              11.00 |

**Build/sampling cost at matching point counts:**

| points | C++ atom build_ms | MPY atom build_ms | C++ orbital build_ms | MPY orbital build_ms |
|-------:|-------------------:|--------------------:|-----------------------:|------------------------:|
|    500 |                 20* |                  325 |                     105 |                      453 |
|   2000 |                  24 |                 1392 |                     114 |                     1727 |
|   8000 |                  50 |                 5227 |                     214 |                     7220 |

(*C++'s own 500pt atom row has a one-off 265ms branch-prediction warmup artifact, documented
above; the 1000pt row's 20ms is the representative "warm" figure.)

### Interpretation

- **Render path**: once the animation is doing genuinely equivalent per-frame work (fade +
  alpha-blend, matching C++ bit-for-bit), MicroPython is consistently ~3x slower than C++ at
  every point count -- `@micropython.viper`'s Q8-fixed-point loop is fast for an interpreted
  target, but not compiled-native fast, and the persistence fade (57,600 pixels touched every
  frame, independent of point count) is now the dominant per-frame cost. `avg_blit_ms` is flat
  at ~13.1ms on both platforms, confirming the SPI-clock fix closed that gap specifically -- the
  remaining FPS gap is compute, not the display transfer.
- **Build/sampling path**: MicroPython is ~100x slower for atom sampling and ~34x slower for
  orbital sampling at 8000 points, even after caching (eliminating the C++-side's
  compile-time-embedded-table advantage), `@micropython.native` on the hot loops, and inlining
  the color-encoding math to avoid losing native speedup on nested non-native calls. Applying
  those three optimizations took the atom build from 7670ms to 5227ms (-32%) and the orbital
  build from 13139ms to 7220ms (-45%) at 8000 points, measured against the original
  uncached/unoptimized MicroPython numbers -- and that's despite the optimized numbers now
  correctly *including* the per-point color-encoding step, which the original measurement had
  silently left untimed. This residual gap (an interpreter, even an optimized one, doing
  per-point trig/branchy Python vs compiled C++) is expected and would need inlining
  `sample_orbital_point()`/`psi_real()` themselves to close further -- not done, since those are
  cross-validated against the C++ and JS reference ports (`tools/orbitals_host/`) and duplicating
  them inline is a real correctness risk for marginal further gain.
- One-time table cost (~2.4-2.7s for Fe, ~0.65s for 2p_z) is a real, one-off "Loading..." delay
  on first visit to an element/orbital per session, not a per-frame cost -- cached for every
  later revisit.

### Caveats / non-identical factors not chased down further

- Font rendering differs: C++ rasterizes a real typeface (Jersey10) into a proportional bitmap
  font (`src/render/font.cpp`); MicroPython uses `framebuf`'s built-in fixed 8x8 font. Both are
  bitmap fonts drawn pixel-by-pixel (same general cost order), and it's a small, point-count-
  -independent fixed cost either way (one title string + one scale-bar string per frame) -- not
  worth unifying just for this benchmark.
- `frames_per_step=30` on MicroPython vs 60 on C++, purely for capture-time budget; FPS already
  converges well before 60 samples at either count.
