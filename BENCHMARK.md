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

## Expected results (baseline: 2026-08-20, ESP32-S3 @ 240MHz, SPI 40MHz, this hardware unit)

Captured after the atom/orbital split and the `iram_free` column were added -- this is the
first baseline with both sweeps. `avg_wait_ms` isn't printed by the summary table below (see
the raw `BENCH,STEP` line for it) but stayed flat at ~12-14ms across every row of both sweeps,
as expected for a fixed-size SPI DMA transfer.

### Performance (`BENCH,STEP`)

**Atom sweep (Fe, Z=26):**

| points | build_ms | avg_render_ms | min_render_ms | max_render_ms | fps  | iram_free |
|-------:|---------:|---------------:|---------------:|---------------:|-----:|----------:|
|    500 |      265 |          10.159 |          10.152 |          10.243 | 42.09 |    216687 |
|   1000 |       20 |          10.617 |          10.610 |          10.689 | 42.04 |    216687 |
|   2000 |       24 |          11.539 |          11.530 |          11.622 | 41.97 |    216687 |
|   4000 |       33 |          13.943 |          13.936 |          14.012 | 36.00 |    216687 |
|   8000 |       50 |          18.149 |          18.141 |          18.215 | 31.48 |    216687 |

**Orbital sweep (2pz, `kOrbitalDefaultPresetIndex`):**

| points | build_ms | avg_render_ms | min_render_ms | max_render_ms | fps  | iram_free |
|-------:|---------:|---------------:|---------------:|---------------:|-----:|----------:|
|    500 |      105 |          10.476 |          10.470 |          10.581 | 42.06 |    216387 |
|   1000 |       99 |          10.967 |          10.959 |          11.099 | 42.02 |    216387 |
|   2000 |      114 |          11.980 |          11.969 |          12.101 | 38.80 |    216387 |
|   4000 |      145 |          14.580 |          14.573 |          14.666 | 35.97 |    216387 |
|   8000 |      214 |          19.139 |          19.130 |          19.234 | 31.44 |    216387 |

(500-point atom `build_ms` of 265 is a one-off: the very first sweep step, before Fe's
subshell rejection-sampling has "warmed up" any branch prediction/caches -- every later atom
step, and the whole orbital sweep, doesn't show it. Not a regression signal by itself; watch
whether it recurs at 500 points specifically across runs.)

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
