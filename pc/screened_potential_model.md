# Screened-potential model (R2/R3) — design for high-Z atom clouds

Status: design + PC implementation (validated against literature). Device
(ESP32) port done (2026-08, C++/ESP-IDF; MicroPython not yet wired up); see
§7 for what landed and what's still open.

## 0. Problem: why the current model breaks at high Z

The current atom model (`micropython/slater.py` + `atom_cloud.py`) replaces
every subshell's radial function with a **hydrogenic** function scaled by a
single effective charge `z_eff_radial()` (Clementi-Raimondi Z_eff where the
table covers it, Slater's rules × n/n* beyond Z=54). ATOMS.md §4.2 measured
the consequences (model/literature radius ratio):

| Z | element | current ratio | why |
|---|---------|--------------:|-----|
| 3 | Li      | 1.30 | hydrogenic shape |
| 11| Na      | 1.45 | hydrogenic shape |
| 26| Fe      | 1.54 | hydrogenic shape |
| 36| Kr      | 1.52 | hydrogenic shape |
| 55| Cs      | 3.34 | hydrogenic shape + Slater fallback |
| 79| Au      | 3.40 | same + missing relativity |
| 92| U       | 4.96 | same + missing relativity |

Three distinct causes, in increasing order of impact for heavy elements:

1. **Hydrogenic radial shape** — even with the *correct* Z_eff, a hydrogenic
   R(r) is too diffuse for n≥2: the real Hartree-Fock radial function must be
   orthogonal to the filled core, so it develops inner oscillations that push
   the maximum of r²R² inward. This is the whole 1.3–1.5× residual of
   periods 2–4 (Li/Be/Na/Fe/Kr in the table).
2. **Slater fallback for Z>54** — Clementi-Raimondi covers Z≤54; beyond it
   Slater's 1930 rules + the n/n* rescaling are crude, producing radii 3–5×
   too large for Cs/Au/U.
3. **Missing relativity** — for Z≳55 the s/p orbitals contract
   (√(1−(Zα)²) scale: ≈8% at Cs, 18% at Au, 26% at U), because the inner
   electron kinetic energy becomes comparable to mc².

A single `Z_eff` cannot express any of these: the real effective charge is a
**function of r** — Z at the nucleus, ~1 at infinity — and the wavefunction
is the eigenfunction of the corresponding central potential, not a scaled
hydrogenic one.

## 1. Method: self-consistent screened-potential (Hartree–Fock–Slater) + Numerov

Replace the "one Z_eff per subshell" model with the classic *central-field
approximation* (the same model behind the standard atomic-structure codes):
build ONE radial potential per element from its own electron density, and
solve every occupied subshell's radial wavefunction in it.

- V(r) = −Z/r + V_ee(r) + V_x(r)   (nuclear + electron–electron + exchange)
- V_ee(r): Coulomb potential of the spherically averaged electron density
  ρ(r) (1D Poisson integral).
- V_x(r): local (Slater) exchange, V_x = −3α(3ρ/8π)^(1/3), α tunable
  (α=1 Slater exchange "HFS", α=2/3 Dirac/Kohn–Sham; scan to match radii).
- **Latter cutoff** (Latter 1955): V(r) := min(V(r), −1/r). The Hartree
  potential wrongly includes the electron's own self-interaction, so its
  tail is −(Z−1)/r instead of the physical −1/r; the cutoff restores the
  correct asymptotic for the outermost electron — the one that sets the
  atom's visual size. [1]
- Solve per (n,l): radial Schrödinger equation (Hartree a.u., r in Bohr)
  with u(r) = r·R(r):
  `u'' = [l(l+1)/r² + 2(V − E)] u`
  as an eigenproblem; eigenfunctions of the *same* potential with different
  n are automatically orthogonal — this produces exactly the core-orthog-
  onality contraction the hydrogenic model lacks (cause #1).
- **Self-consistency**: start from the current model's density (CR-Z_eff
  hydrogenic), build V, solve all occupied subshells, rebuild ρ from the
  solutions, mix, iterate until the eigenvalues/density converge.
  (Historical reference implementation: Herman & Skillman 1963. [2])

### 1.1 Numerical core

- **Grid**: log-uniform, r_i = r₀·e^{t_i}, t_i uniform (r₀ ≈ 1e−5 a₀,
  r_max ≈ 60 a₀, N ≈ 4000). Constant *relative* resolution everywhere —
  needed because U's 1s lives at ~0.01 a₀ while Fr's 7s tail reaches
  ~10–20 a₀.
- With v(t) = r^{1/2}·R(r) the equation becomes a clean 1D Schrödinger
  form with no first-derivative term:
  `v'' = [l(l+1) + 1/4 + 2(V − E) r²] v`
  → symmetric tridiagonal generalized eigenproblem `A v = E B v`,
  B = diag(2r²). With w = B^{1/2}v the standard tridiagonal problem
  `(B^{-1/2} A B^{-1/2}) w = E w` (still tridiagonal) is solved with
  `scipy.linalg.eigh_tridiagonal`, keeping only the n_occ lowest states of
  each l. O(N·n_occ), milliseconds per subshell — fast enough for an
  offline all-Z table.
- **Validation gate for the machinery**: with an empty density the
  potential is exactly −Z/r, so the solver must reproduce hydrogen exactly:
  E_n = −Z²/(2n²), mode of r²R² at n²/Z a₀. This is asserted in the
  harness (see §5).

## 2. Relativity (R3): radial Dirac equation for Z ≳ 55

The nonrelativistic Schrödinger solution is progressively wrong past the
4th period. The physically correct fix within the same central-field
framework is the **radial Dirac equation** (same V(r)):

```
dg/dr = −(κ/r) g + (2c + (ε−V)/c) f
df/dr =  (κ/r) f − ((ε−V)/c) g
```

with c = 137.036 a.u., κ = −(l+1) for j = l+½, κ = l for j = l−½, ε the
binding energy, g/f the large/small components times r. Solved by outward
shooting + bisection on ε with node counting (nodes of g = n−|κ|−1). The
two j-split radial functions per (n,l>0) are merged density-weighted
(2j+1) for the visual model. Validation target: exact hydrogenic Dirac
energies for Z=1 and the documented contractions (6s Au ~18%, 7s U ~26%).

Reference: Dirac-Fock atomic tables (Desclaux 1973) [3]; NIST "Atomic
Reference Data for Electronic Structure Calculations" (Kotochigova et al.
1997) [4] provides relativistic LDA wavefunctions for Z=1..92 as an
independent cross-check (downloaded by the user, see §5).

## 3. Output / data flow

```
pc/hfs_solver.py            offline, numpy: SCF screened-potential solver
                            → pc/hfs_tables.npz  (per Z, per (n,l): r-grid,
                              u(r)=rR(r), eigenvalue ε, flags)
pc/hfs_tables.py            loader: radial source objects for the samplers
micropython/pointcloud.py   + init_radial_sampler_from_table() /
                              radial_fn injection (unchanged default path!)
micropython/atom_cloud.py   + optional tabulated-radial path (flag/model)
pc/atom_view_pc.py          --model hfs|hydrogenic flag (A/B on screen)
pc/validate_atoms.py        --model hfs: new radial source in all checks
```

The hydrogen preset path (`z_eff=1.0`, `cloud_common.py`) is untouched:
new code is additive, old default behavior bit-identical.

## 4. What the sampler needs (interface, not format)

The point-cloud samplers only ever consume **r²R² as a PDF** (inverse-CDF
tables built by `pointcloud._build_inverse_cdf`). The new model therefore
needs, per (Z,n,l): a radial function R(r) (for the mode/radius, the
isotropic table, and the Hund-sign recomputation) plus a sensible max_r.
Two interchangeable back-ends:

1. **PC validation/debug**: dense grid u(r) from the npz, cubic interp.
2. **Device (final goal)**: compact STO fit of each u(r)
   (u ≈ Σ_i c_i r^{p_i} e^{−ζ_i r}, p_i = l+1+i, ~6–10 terms) or a ~64-point
   log-grid + Hermite interpolation; either one feeds the SAME
   `_build_inverse_cdf` at load time (already the device pattern for the
   hydrogen presets — the ESP32 builds inverse-CDF tables at init, it never
   stores them). Size estimate: ~10 floats × ~15 subshells × 118 elements
   ≈ 15–20 KB as flash PROGMEM for the STO form.

## 5. Validation plan (PC, against literature)

1. **Solver self-check**: hydrogen limit (empty density) — E and modes
   exact vs analytic (hard gate, `pc/hfs_solver.py --coulomb-check`).
2. **Dirac machinery gate**: exact hydrogenic Dirac energies
   (`pc/dirac_solver.py --check`) — all states to 1e-9.
3. **Clementi-Raimondi atomic radii** (in-repo `pc/clementi_radii.py`,
   Z=1..54 + lanthanides/actinides): mode of valence r²R² vs table —
   primary quantitative gate. Measured at α=1.0 (Slater exchange) on the
   representative set: H 0.95, Li 0.89, C 0.83, Na 0.88, Fe 0.80,
   Kr 0.86, Xe 0.88, Cs 0.96, Au 0.91, U 1.32 (vs 1.30 / 1.45 / 1.54 /
   1.52 / 3.34 / 3.40 / 4.96 today). The residual ~0.8–0.9× is a smooth,
   systematic Xα bias — unlike the current model's 1.3–5× errors.
   **α choice (settled by the NIST data)**: comparing our valence
   eigenvalues against the NIST LDA table (Kotochigova et al., which
   includes correlation) shows α=1.0 is ~1.2–3 eV too deep, while
   **α=2/3 (Dirac/Kohn–Sham exchange) matches the NIST LDA eigenvalues to
   <0.7 eV** (Ar −0.04 eV, Kr +0.33, Xe +0.66). The NIST configurations
   file matches slater.electron_configuration() 92/92. **Final choice:
   α=2/3.** The earlier concern that α=2/3 inverts the transition-metal
   3d/4s ordering (Fe 3d drifting out to 420 pm) turned out to be an SCF
   mixing artifact, not physics: with the default 50% density mixing the
   SCF chases an unstable direction into a metastable diffuse-3d solution;
   with the ARPACK warm-start sigma bug fixed (see below) the SCF is stable
   and mix-independent (0.3–0.5 agree), giving the physical solution
   (Fe 3d 36 pm inside 4s 140 pm, ε(3d) −0.268 vs NIST LDA −0.295),
   so **mix=0.4 (speed) is the default, warm_start off**. **Actual root
    cause found and fixed (2026-08-19)**: the per-l ARPACK warm-start
    shift (previous iteration's deepest eigenvalue − 0.05) could land
    ABOVE an occupied state when eigenvalues move between iterations
    (Fe 3d: −0.06 → −1.35 Ha), so ARPACK returned an UNOCCUPIED
    eigenvalue in its place and the corrupted density drove the
    collapse. Removed. Final radii at α=2/3
   on the representative set: H 0.99, He 0.92, Li 0.95, Be 1.03, C 0.91,
   Ne 0.81, Na 0.94, Ar 0.92, Cr 0.95, Fe 0.90, Cu 0.96, Kr 0.91,
    Xe 0.93, Cs 0.99, Au 0.95 — all
   within ±10–20% of Clementi, versus the old model's 1.3–5× errors.
    U (NR) is 1.39; with the relativistic (Dirac) final pass the 7s
    contracts to 173.7 pm vs literature 175 → **0.99**. The 5d/6s block
    (Hf–Au) reads ~0.7 vs Clementi: the tables are RELATIVISTIC for
    Z≥55, while Clementi-Raimondi is nonrelativistic -- the s/p
    contraction is real (validated against the NIST RLDA eigenvalues) and
    the offset is a reference mismatch, not an error. Pd (Z=46) is a
    documented Xα/LDA d-shell limitation (eigenvalue matches NIST LDA;
    density too compact vs HF).
4. **NIST "Atomic Reference Data for Electronic Structure Calculations"
   (Kotochigova et al. 1997) [4]**: per-element files (Z=1..92) with total
   energies + orbital eigenvalues in four approximations (LDA, LSD, RLDA,
   ScRLDA). User-provided archive `dftdata.tar.gz` — `pc/nist_compare.py`
   compares (a) our HFS eigenvalues vs LDA (matched to <0.7 eV at α=2/3),
   (b) our DIRAC eigenvalues vs RLDA, including the spin-orbit splits
   ε(nlM) − ε(nlP) vs ε(κ=+l) − ε(κ=−(l+1)) — measured at 1.2–1.4× the
   NIST splits at α=1.0 (potential too deep); re-measuring at α=2/3,
   (c) the configurations file vs slater.electron_configuration()
   (**92/92 match**).
5. **Koopmans/IP check**: −ε of the highest occupied orbital vs
   experimental first ionization energy (NIST SRD 111 [5]; curated
   high-confidence subset embedded in `pc/validate_atoms.py`).
6. **Clementi–Roetti RHF wavefunctions Z≤54 [6]** (if obtainable): direct
   comparison of r²R² shapes/modes with true HF.
7. **Existing physics checks unchanged** (Unsöld isotropy, Hund anisotropy,
   Fe 3d<4s ordering, H/He exactness) — must keep passing with the new
   radial functions (`pc/validate_atoms.py --model hfs --strict`).

## 6. References

- [1] R. Latter, *Phys. Rev.* **99**, 510 (1955) — Thomas–Fermi/–Dirac
  energies with the −1/r potential cutoff.
- [2] F. Herman & S. Skillman, *Atomic Structure Calculations*
  (Prentice-Hall, 1963) — Hartree–Fock–Slater SCF with Numerov, the
  reference implementation of §1.
- [3] J. P. Desclaux, *At. Data Nucl. Data Tables* **12**, 311 (1973) —
  relativistic Dirac–Fock orbital radii/energies, all elements.
- [4] S. Kotochigova, Z. H. Levine, E. L. Shirley, M. D. Stiles, C. W.
  Clark, "Atomic Reference Data for Electronic Structure Calculations",
  NIST (1997), https://math.nist.gov/DFTdata/atomdata/ — relativistic LDA
  wavefunctions/densities for Z=1..92.
- [5] A. Kramida et al., NIST SRD 111 "Ground Levels and Ionization
  Energies for the Neutral Atoms", https://physics.nist.gov/PhysRefData/IonEnergy/
- [6] E. Clementi & C. Roetti, *At. Data Nucl. Data Tables* **14**, 177
  (1974) — Roothaan-Hartree-Fock STO expansions, Z≤54.
- [7] E. Clementi, D. L. Raimondi, *J. Chem. Phys.* **38**, 2686 (1963);
  E. Clementi, D. L. Raimondi, W. P. Reinhardt, *J. Chem. Phys.* **47**,
  1300 (1967) — Z_eff tables and atomic radii (already in-repo).

## 7. Device (ESP32) port plan

1. PC: solver → validated tables (this milestone). **Done (2026-08)** —
   SPARC-atomSFE LDA_SVWN tables, Z=1..92, NIST-exact eigenvalues (see
   pc/RUN_HFS.md §5; pc/hfs_atomsfe.py).
2. PC: compact per-subshell representation + accuracy check vs the dense
   table. **Done, superseded plan** — the 128-pt log-grid reference
   (`pc/hfs_tables_reduced.npz`) is committed (valence modes within 1.5% of
   the full tables; `pc/hfs_tables.py --compact ... 128`) and, per step 3
   below, turned out to be the FINAL device form, not an intermediate one:
   the STO fit / 64-pt-Hermite compaction this step originally planned is no
   longer needed (see step 3's size note) and the sampled-distribution KS
   check was superseded by the direct numerical cross-check described there.
3. Firmware. **Done (2026-08), BOTH C++/ESP-IDF and MicroPython** —
   `tools/hfs_table_gen.py` packs the 128-pt reduced npz into ONE binary blob
   (little-endian `struct`: shared `r` grid + per-Z element index +
   per-subshell `(n, ell)` index + `u(r)` rows), written to two identical
   copies read ON DEMAND from flash/filesystem rather than compiled into
   either firmware image as source-level data (an earlier iteration of this
   work compiled it directly into `src/hfs_tables.h` as ~470 KB of `.rodata`;
   moved off that to keep the data out of the firmware image/OTA payload —
   see the size/perf note below):
   - `data/hfs_tables.bin` → flashed to the `storage` SPIFFS partition
     (`partitions_16M.csv`) via `pio run -t uploadfs` (separate from the
     normal firmware flash, see pc/RUN_HFS.md's device note for the exact
     command). `src/hfs_radial.cpp` (hand-written; `src/hfs_tables.h` is now
     GENERATED but tiny — just the three size constants, data/logic split
     still mirrors `font.h`/`font_data.h`, CLAUDE.md §4.1) mounts `/storage`
     (idempotent, shares the partition `screenshot.cpp` already mounts),
     loads the small header/index once, and reads each subshell's `u(r)`
     row on demand. Direct port of `pc/hfs_tables.py`'s
     `RadialSource`/`HfsTables` and `micropython/atom_cloud.py`'s
     `radial_tables` branch onto that reading strategy.
   - `micropython/hfs_tables.bin` → deployed to the device root the normal
     way (`mpremote ... fs cp -r micropython/. :`).
     `micropython/hfs_radial_tables.py` (hand-written) reads it the same
     way (header/index resident, rows on demand via `open()`/`seek()`).
     `micropython/atom_view.py` now loads it once and passes it as
     `build_atom_point_cloud()`'s `radial_tables` argument, and
     `micropython/hfs_atom_size_calib.py` (generated, table-based factors)
     replaced the hydrogenic `atom_size_calib.py` for that one caller —
     `atom_size_calib.py` itself is left untouched since
     `pc/atom_view_pc.py`'s hydrogenic default and the web viewer still
     depend on it staying hydrogenic.

   `src/pointcloud.h` gained the missing piece, `buildInverseCdfFromGrid()`
   (arbitrary/log-grid trapezoidal CDF, a port of
   `micropython/pointcloud.py`'s `_build_inverse_cdf_from_grid()`,
   generalized with an output resolution independent of the source table's
   own point count) plus `interpOnGrid()` (port of `interp_u()`);
   `src/atom_cloud.cpp` selects the tabulated radial source per (Z,n,l) for
   z<=92 when the table is available, falling back to the hydrogenic model
   both for z outside that coverage and for a board that hasn't run
   `pio run -t uploadfs` yet (`hfsFindU()` returns `nullptr` either way).
   Cross-checked against an independent NumPy re-derivation of the same
   trapezoidal-CDF algorithm: the C++ (float64 build) and Python quantile
   outputs agree to ~1e-8 relative on representative subshells (Fe 4s, U 1s)
   — same cross-port numerical discipline as `tools/orbitals_host`, done ad
   hoc here rather than extending that harness (which doesn't compile
   `atom_cloud.cpp`/`hfs_radial.cpp` at all, since both pull in ESP-IDF-only
   headers). The MicroPython loader/sampler pipeline was verified end to end
   under the real MicroPython 1.17 unix-port interpreter (not just the
   CPython shim), including a byte-exact check of the file reads against the
   source npz.
4. On-device benchmark + screenshot A/B vs PC. **Not done** — no change to
   per-point sampling cost (one interpolation + one inverse-CDF lookup
   either way, as originally estimated, PLUS now one file open/seek/read per
   subshell per element switch — still not per-frame or per-point), but
   this hasn't been measured on real hardware yet or compared frame-for-frame
   against `pc/atom_view_pc.py --model hfs`.

Size/perf: **the original +15–20 KB (STO form) budget no longer applies, and
the ~470 KB raw-table budget that superseded it (committed 2026-08) doesn't
either** — the table now lives in the `storage` SPIFFS partition (7 MB, only
lightly used by on-device screenshots) as `data/hfs_tables.bin`, read on
demand, rather than compiled into either firmware image, so this section's
original "flash +X KB" framing no longer applies at all: the OTA/firmware
image itself carries none of this data now. RAM: each sampler build uses a
few KB of function-local `static` scratch (same class as the existing
hydrogenic `buildRadialSamplerRuntime()`), plus a few KB of resident
header/index loaded once by `hfsInit()`/`hfs_radial_tables.HfsTables()` —
no PSRAM attribute needed, these are small. No change to the per-frame
sampling loop; the one new cost is a file open/seek/read per subshell, paid
once per element switch on both ports.
