# Rendering benchmark

`src/benchmark_test.h`/`.cpp` sweeps a fixed atom (Fe, Z=26) across several point-cloud sizes,
timing real production-path rendering at each size, and logs both performance numbers and a
handful of deterministic physics numbers usable as a correctness regression check. See that
file's header comment for the full rationale.

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

The sweep takes ~8-9 seconds total (5 point-count steps x 60 frames each, plus point-cloud
build time per step) and needs no IMU/tilt setup or user interaction.

## Expected results (baseline: 2026-08-19, ESP32-S3 @ 240MHz, SPI 40MHz, this hardware unit)

Updated 2026-08-19 after the bitmap-font rewrite (proportional Jersey10 fonts, see
`tools/font_gen/`): `avg_render_ms` dropped ~5-8% at the higher point counts because the
scale bar's label switched from `drawTextScaled()` (a per-pixel scaling loop) to plain
`drawText()` at `kFontLarge`'s native size -- both drawn every frame, same as production.
Physics (`CONFIG`/`ZEFF`/`GEOM`) is unaffected and stayed bit-identical, as expected since
that commit never touched `slater.h`/point sampling.

### Performance (`BENCH,STEP`)

| points | build_ms | avg_render_ms | min_render_ms | max_render_ms | avg_wait_ms | fps  |
|-------:|---------:|---------------:|---------------:|---------------:|------------:|-----:|
|    500 |       30 |           9.68 |           9.68 |           9.77 |       12.09 | 45.9 |
|   1000 |       32 |          10.08 |          10.08 |          10.15 |       11.72 | 45.9 |
|   2000 |       36 |          10.88 |          10.87 |          10.96 |       12.89 | 42.1 |
|   4000 |       44 |          13.07 |          13.06 |          13.14 |       12.70 | 38.8 |
|   8000 |       62 |          16.83 |          16.82 |          16.90 |       12.92 | 33.6 |

Notes:
- **8000 points is the production count** (`kAtomNumPoints`, what `atom_view.cpp` actually
  renders) -- the number that matters is the last row: **~33-34 FPS**, comfortably above the
  20-30 FPS target in `CLAUDE.md` §6.
- `avg_wait_ms` (blocked on the previous frame's SPI DMA) should stay roughly flat (~11-12ms)
  across all point counts -- it's dominated by the fixed 240x240x16bit frame transfer, not by
  point count. If it scales with point count instead, something changed in how/when
  `presentFrame()`/`waitForFlushDone()` are called.
- `avg_render_ms` (CPU: fade + rotate/project/rasterize + text) should scale roughly linearly
  with point count. A large jump in the 500-point row specifically (which should be cheapest)
  usually means a fixed per-frame cost (e.g. `fadeFrameBuffer()`, title/scale-bar text) grew,
  not a point-cloud regression.
- `build_ms` (point-cloud sampling) growing faster than linearly with point count would suggest
  a regression in the radial/angular sampling rejection rate, not just raw point count.
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
|    500 | 4s (n=4, ell=0) |         6.004462 |      12.490710 |
|   1000 | 4s (n=4, ell=0) |         6.259300 |      11.982170 |
|   2000 | 4s (n=4, ell=0) |         6.147317 |      12.200443 |
|   4000 | 4s (n=4, ell=0) |         6.136905 |      12.221145 |
|   8000 | 4s (n=4, ell=0) |         6.164497 |      12.166442 |

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
