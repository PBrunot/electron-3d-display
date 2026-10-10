# Physics correctness review

## Current upstream main: follow-up verification

On 2026-10-10, checked `fablab-bergamo/electron-3d-display` main at
[`aa5a77baefd986fc56322f0cd8aead017e66d60e`](https://github.com/fablab-bergamo/electron-3d-display/commit/aa5a77baefd986fc56322f0cd8aead017e66d60e).
The original review below concerns this workspace's different checkout:
`PBrunot/electron-3d-display`, master at `bea99a8`. Its old numerical results
must not be read as measurements of upstream main.

| Original finding | Status on upstream main |
| --- | --- |
| Extra radius factor in atomSFE export | Fixed by [`d1d572d40a73`](https://github.com/fablab-bergamo/electron-3d-display/commit/d1d572d40a73). Export, origin behavior, NPZ, binary copies, and calibration factors were updated. |
| HFS calibration applied to hydrogenic fallback | Still present: `atom_cloud.cpp` selects fallback radii but applies the table-based factor unconditionally. |
| Uniform-grid CDF bias and different radial paths | Still present. Remeasured against the corrected main assets; see below. |
| HFS generator writes its header to the old location | Fixed by [`6100db79c3a5`](https://github.com/fablab-bergamo/electron-3d-display/commit/6100db79c3a5). |
| Old JS/C++ cross-check paths and C++17 build | Still stale in the legacy harness. Main now has a separate working host test suite and physics CI, added by [`eb0cbcbbf4af`](https://github.com/fablab-bergamo/electron-3d-display/commit/eb0cbcbbf4af). |

The peak-radius bounding circle also landed in these fixes. Main carries
`peakR` separately from the p90 framing radius in both the atom and subshell
dissection paths. The circle represents the calibrated valence mode.

Verification used a pinned archive extracted under
`/tmp/electron-physics-review/main`, leaving this checkout unchanged:

- 32 tests passed in `test_hydrogenic.py` and `test_device_physics.py`.
- 8 further tests passed: generated-asset consistency, calibrated radii,
  energy ordering, 1s and valence radius checks, and sampled-table radii.
- The 3 remaining cases were excluded from that invocation: the row
  normalization test uses NumPy's newer `trapezoid` API, and the two
  independent-LDA cases require SciPy. This environment has NumPy 1.23.2
  and no SciPy. Normalization was checked independently using `trapz`:
  all 915 rows pass, with maximum error 5.23e-8. The two independent-LDA
  tests were not rerun locally.
- The original temporary C++/NumPy audit was rerun against main's code and
  assets. Binary/NPZ equality, descriptor order, finite monotone quantiles,
  formula checks, and configuration consistency still pass.
- Main's stored H mode is 1.0613855 a0, rather than the old 2.2752240 a0.
  Li's normalized 1s/2s overlap is now -2.23e-5, rather than -0.12150.

Remaining fallback examples with **main's current** factors:

| Element | Analytic fallback mode after current calibration, pm | Target, pm |
| --- | ---: | ---: |
| H | 49.93 | 53 |
| Li | 217.20 | 167 |
| Fe | 289.99 | 156 |
| Cs | 1141.50 | 298 |
| U | 707.00 | 175 |

These discrepancies concern missing/unreadable HFS assets. The new
device tests explicitly assert that HFS tables loaded successfully, so
they do not exercise the fallback calibration branch.

The uniform-density counterexample remains unchanged. With the corrected,
more contracted radial tables, the worst active oriented-subshell CDF
discrepancy is now **0.06739 for Tm 4f** (6.74 percentage points). Its median
is 0.60547 a0 versus a dense reference of 0.65595 a0, about 7.7% inward.
Fe 3d has discrepancy 0.04253 and median 0.92495 versus 0.97418 a0.
The new sampled-table regression covers Ne and Fe median/p90 with a 6%
relative tolerance; this permits the measured Fe median bias and does not
cover Tm. The direct-grid radial path performs much better for that Tm
subshell (CDF discrepancy 0.00497).

Thus the major export error and generator-path issue are resolved on main.
The remaining priorities are consistent calibration during fallback and
consistent radial CDF construction, with regressions for a constant density,
missing assets, and contracted open f subshells.

## Original local-checkout review (historical)

Reviewed 2026-10-10, master at `bea99a8`. Scope: the C++ physics pipeline,
committed orbital/HFS assets, their Python generators, and host validation.
Firmware and generated assets were not modified. No PlatformIO build, flash,
monitor, or hardware test was performed.

The hydrogenic formulas are sound in the tested range, but the atom viewer's
tabulated radial density has a substantial generation error. Correcting the
generation and regenerating the tables takes priority over tuning the display.

## Findings

### P1: atomSFE orbitals are multiplied by radius twice

Location: `pc/hfs_atomsfe.py:107`.

`solve_element()` assumes `res['orbitals']` contains the ordinary radial
function R(r), and constructs `u_src = rq * orb[:, i]`. Upstream actually
returns the reduced radial function u(r) = r R(r). Its solver passes through
`scf_result.orbitals`; its density calculator divides these orbitals by r
before squaring them. See the upstream
[solver](https://github.com/SPARC-X/SPARC-atomSFE/blob/main/src/solver.py) and
[density implementation](https://github.com/SPARC-X/SPARC-atomSFE/blob/main/src/scf/density.py),
especially `DensityCalculator.compute_density()`.

Consequently the committed arrays contain a normalized version of r u(r),
rather than u(r). Sampling their square gives radial probability proportional
to r^4 R(r)^2 instead of r^2 R(r)^2. Renormalizing the arrays conserves total
probability but does not correct their shape.

Independent checks of `pc/hfs_tables_reduced.npz` corroborate the convention
error:

| Check | Stored tables | Expected / diagnostic correction |
| --- | ---: | ---: |
| H 1s near-origin power, r=0.001..0.01 a0 | 1.976 | approximately 1 |
| C 2p near-origin power, same interval | 2.933 | approximately 2 |
| Fe 3d near-origin power, same interval | 3.864 | approximately 3 |
| Li normalized 1s/2s overlap | -0.12150 | -0.0000223 after dividing stored arrays by r |
| Fe normalized 1s/2s overlap | -0.21767 | -0.0002455 after dividing stored arrays by r |
| H radial probability mode | 2.27522 a0 | 1.06139 a0 after dividing stored array by r |

The diagnostic division is evidence, not a proposed replacement for proper
regeneration: interpolation and normalization should be repeated from the
solver's original output.

Eigenvalue agreement does not detect this error because energies are copied
separately from `res['eigen_energies']`. Spot checks of H 1s, Fe 3d/4s, and
U 5f/7s agree closely with the corresponding NIST LDA values, even though the
stored radial functions are wrong. References:
[H](https://math.nist.gov/DFTdata/atomdata/tables/01H.html),
[Fe](https://math.nist.gov/DFTdata/atomdata/tables/26Fe.html),
[U](https://math.nist.gov/DFTdata/atomdata/tables/92U.html).

The per-element size calibration forces one mode radius back to its target,
concealing this deformation. It cannot restore the correct radial density,
relative core/valence extents, or orthogonality. The binary HFS assets match
the erroneous NPZ exactly, so the error reaches the device.

Recommended fix: use the solver's reduced radial orbitals directly, verify
their normalization and same-ell orthogonality, then regenerate full/reduced
tables, both binary copies, and all table-based size factors. Add an export
check against the solver's own spherical density, rather than relying only
on energies or calibrated radii.

### P2: the hydrogenic fallback uses calibration for a different model

Locations: `src/physics/atom_cloud.cpp:113` and `:159`;
`src/physics/atom_size_calib.h`.

Missing storage, missing assets, or a failed HFS row read triggers the
hydrogenic model. The cloud still receives `kAtomSizeCalibFactor[z-1]`, whose
generator explicitly computes CR radius / tabulated-model radius.

For a fully unavailable HFS asset, the analytic fallback modes multiplied
by the current factors are:

| Element | Resulting valence mode, pm | Configured reference target, pm |
| --- | ---: | ---: |
| H | 23.29 | 53 |
| He | 13.90 | 31 |
| C | 37.63 | 67 |
| Fe | 213.42 | 156 |
| Cs | 879.53 | 298 |
| U | 573.09 | 175 |

These are analytic hydrogenic modes with the actual C++ effective charges
and factors; finite-table sampling adds its own smaller error. Auto-fit can
hide the overall size discrepancy, while scale-bar units and dissection
distances remain incorrect.

Recommended fix: select calibration alongside the radial model. For a
partially failed asset, avoid silently mixing radial models under one global
factor; switch the whole atom consistently or define and validate the mixed
model explicitly. Regenerate factors after addressing P1.

### P2: the uniform-grid CDF builder introduces a systematic inward bias

Location: `src/physics/pointcloud.h:100`;
`src/physics/hfs_radial.cpp:154`.

`buildInverseCdf()` includes the first weight in the cumulative sum and
places that mass at coordinate zero. It then interpolates the cumulative
values as though they were a continuous CDF on the grid. Even a constant
density does not produce the required identity quantile function:

```
weights = [1, 1, 1, 1, 1], domain = [0, 1]
expected quantiles: [0, 0.25,   0.5,   0.75,   1]
actual quantiles:   [0, 0.0625, 0.375, 0.6875, 1]
```

On the 1001-point hydrogenic assets this is a small approximation error
(worst measured radial CDF discrepancy about 0.00363 for 6s/6pz). On the
oriented HFS path the grid spans 0..100 a0 with spacing 0.1 a0, making the
bias more significant for contracted subshells.

Against a dense integration of the committed table's linearly interpolated
u(r)^2, measured active open-subshell discrepancies include:

| Subshell | Maximum CDF discrepancy | Actual median / reference median, a0 |
| --- | ---: | ---: |
| H 1s | 0.01649 | 2.64566 / 2.69558 |
| Fe 3d | 0.02300 | 1.75552 / 1.80501 |
| Gd 4f | 0.03578 | 1.17363 / 1.22285 |

The isotropic and oriented paths target the same radial marginal but use
different numerical constructions. Their differences should not follow
whether a subshell happens to be full. A comment in `hfs_radial.h` preserves
the asymmetry for fidelity to Python; matching ports does not establish
physical correctness.

Recommended fix: integrate intervals from a zero CDF using a consistent
quadrature rule. Reuse a common table-fed radial sampler for open and closed
subshells; keep the angular sampling separate. Update the Python ports and
regenerate the hydrogenic sampler asset if its CDF construction changes.
These measurements apply to the current assets and should be repeated after
the P1 correction.

### P2: validation and regeneration have stale paths

Locations: `tools/orbitals_host/run_crosscheck.sh:26`,
`tools/orbitals_host/gen_c_reference.cpp:11`, and
`tools/hfs_table_gen.py:138`.

The host script compiles `src/orbitals.cpp` and `src/pointcloud.cpp`, which
have moved to `src/physics/`. The C++ reference generators also include bare
`orbitals.h` / `pointcloud.h`. Separately, the HFS generator writes its size
header to `src/hfs_tables.h`, while the firmware consumes
`src/physics/hfs_tables.h`. A regenerated blob can therefore leave the
firmware's constants stale.

With the system compiler, C++17 also rejects the uninitialized local
`uPowJ` declaration inside the current constexpr `computePLM()`. The
temporary review harness compiled successfully in C++23. Either restore
the claimed C++17 compatibility or update the host build instructions.

Recommended fix: update source/include/output paths and the host language
standard before using the existing cross-check as a verification gate.

## Checks that passed

- Independent generalized-Laguerre finite-sum checks against C++ radial
  evaluations: 22 (n, ell) pairs, n=1..7 and ell<=3, 601 radii each. Maximum
  absolute error divided by the reference function's peak was 3.55e-6.
- Independent associated-Legendre checks against C++ angular evaluations:
  all 16 real s/p/d/f angular functions, 401 angles each. After fitting their
  arbitrary scalar normalization/sign, maximum peak-relative error was
  3.27e-7. The real sine/cosine basis is appropriate for the labeled shapes.
- Electron configurations for Z=1..118 conserve electron count, obey
  subshell capacity, and contain no duplicate subshells. All 92 displayed
  configurations match the NPZ configurations. This is an internal
  consistency check, not a fresh external certification of all 118 ground
  configurations.
- All 915 HFS binary rows and the shared grid match the committed NPZ at
  float32 precision. All 36 hydrogenic binary records match descriptor order;
  their quantile tables are finite and monotone.
- The samplers include the radial r^2 and angular sin(theta) volume factors.
  Independent marginal sampling is justified by separability. The finite
  lookup tables, interpolation, and radial truncation remain numerical
  approximations; comments describing the implementation as exact overstate
  its accuracy.
- Phase colors use the evaluated radial and angular wavefunction signs.
  The comment that all s orbitals are single-signed is false for n>1, but
  the implementation correctly includes their radial phase changes.

Formula references: [NIST DLMF hydrogen/Coulomb functions](https://dlmf.nist.gov/18.39.ii)
and [spherical harmonics](https://dlmf.nist.gov/14.30).

## Model choices and limits

The table source is spin-unpolarized, nonrelativistic LDA, rather than an
exact many-electron solution or the older relativistic HFS solver. NIST
agreement certifies a specified approximation. A global CR scale is an
empirical display calibration; it changes every shell's absolute physical
radius and does not preserve an unmodified LDA density.

Hund filling of a fixed real-orbital basis is useful for demonstrating
occupation. It does not determine a unique ground-state spatial density for
an isolated open-shell atom, nor represent averaging over magnetic substates
of an unpolarized ensemble. Interpret the anisotropy as the selected
independent-electron illustration. Full and equal-occupancy complete angular
bases have spherical summed density.

The radius/effective-charge datasets are transcribed from secondary sources.
Their complete numerical provenance was not independently certified here.
The Python statement that Clementi-Raimondi charges are defined as
`n*sqrt(-2E)` also needs a primary-source check; hydrogenic energy-derived
charges and SCF screening/exponent parameters should not be conflated.

## Verification method and remaining work

Temporary review files are in `/tmp/electron-physics-review/`: `audit.cpp`,
platform logging/storage stubs, `check.py`, `table_shape.py`, and the exported
`results.json`. The C++ harness calls the actual current orbital, Slater,
and HFS radial functions. Only file access is redirected from the device's
`/storage/hfs_tables.bin` to the repository asset. Numerical reference
integrals use dense grids and NumPy; they do not duplicate the existing JS
port as the scientific oracle. These temporary files are not tracked.

No solver regeneration or complete 915-energy NIST rerun was performed:
the vendored solver, local NIST archive, and SciPy were absent. The upstream
solver/density sources were inspected read-only. Formula checks, binary
consistency checks, and the sampling audit ran on the host.

After fixes: rerun independent density/orthogonality and quantile checks,
repair and run the existing cross-port harness, then request user-run
PlatformIO builds for both boards. Device screenshots should verify phase
nodes, relative shell sizes, scale bars, and fallback behavior. Measure
performance only if the changed sampling path shows a regression.
