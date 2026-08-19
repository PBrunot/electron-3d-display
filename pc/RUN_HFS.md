# HFS screened-potential atom model — manual run & verify (runbook)

Design/validation narrative: `pc/screened_potential_model.md` (and ATOMS.md §5).
Prerequisites: Python 3.10+, `numpy`, `scipy`. The `micropython/` modules need
the `micropython_shim` (auto-imported by the pc/ scripts).

---

## 1. Solver self-checks (seconds)

```bash
python pc/hfs_solver.py --coulomb-check   # exact hydrogen: E + modes ~1e-5, all OK
python pc/dirac_solver.py                 # exact Dirac hydrogenic: ~1e-9, all OK
```

## 2. Generate the radial tables (~90–150 min serial)

```bash
python pc/hfs_solver.py --zmin 1 --zmax 118 \
    --alpha 0.6666667 --relativistic --out pc/hfs_tables.npz
```

- `--alpha 0.6666667` = Dirac/Kohn–Sham exchange — the value that matches the
  NIST LDA eigenvalues to <0.7 eV (α=1.0/Slater is ~1.2–3 eV too deep).
- `--relativistic` = one-shot radial Dirac pass on the converged potential
  for Z ≥ `--rel-min` (default 55) — the s/p contraction that fixes U
  (7s: 242 pm NR → 174 pm vs literature 175).
- Progress: one flushed line per element (Z, time, iterations, valence
  mode) + **incremental npz save after every element** — interruption loses
  at most the element in flight. Resume a broken run with `--resume`
  (skips the Z already present in `--out`).
- Optional 4× compaction (513 pts/shell, <0.25% max mode deviation; the
  samplers and harness read both formats):

```bash
python pc/hfs_tables.py --compact pc/hfs_tables.npz pc/hfs_tables.npz 513
```

## 3. Validation against literature (minutes)

```bash
# 3a. Clementi-Raimondi radii + physics checks + Koopmans IP check.
#     Exit 0 = STRICT PASS + every physics check PASS.
python pc/validate_atoms.py --model hfs --strict --all --tables pc/hfs_tables.npz

# 3b. NIST dftdata cross-check (archive under examples/nis data/dftdata,
#     from math.nist.gov/DFTdata/atomdata/):
#       - configurations: 92/92 match slater.electron_configuration()
#       - LDA valence eigenvalues: within <~1 eV at alpha=2/3
#       - RLDA spin-orbit splits: ours ~1.1–1.3× NIST (central-field OK)
#       - ScRLDA (scalar-relativistic) vs our j-averaged Dirac: same
#         construction (NIST ScRLDA == their own RLDA j-average to 0.3%);
#         absolute eigenvalues ~2–10% deeper (exchange-only vs LDA+corr.)
python pc/nist_compare.py "examples/nis data/dftdata" --alpha 0.6666667 \
    --elements 18,36,54,79,92

# 3c. Hydrogenic-model regression (must stay unchanged)
python pc/validate_atoms.py --strict
```

## 4. Interactive PC viewer (Tk)

```bash
python pc/atom_main.py 26 --model hfs     # iron, new radial model
python pc/atom_main.py 92 --model hfs     # uranium (relativistic 7s)
python pc/atom_main.py                    # default: carbon
```

## Expected numbers (representative set, α=2/3, mix=0.35)

| Z  | el | valence | model/lit radius | Z  | el | valence | model/lit |
|----|----|---------|-----------------:|----|----|---------|----------:|
| 1  | H  | 1s      | 0.99             | 36 | Kr | 4p      | 0.91      |
| 3  | Li | 2s      | 0.95             | 54 | Xe | 5p      | 0.93      |
| 6  | C  | 2p      | 0.91             | 55 | Cs | 6s      | 0.93      |
| 11 | Na | 3s      | 0.94             | 79 | Au | 6s      | ~0.95     |
| 24 | Cr | 4s      | 0.95             | 79 | Au | 6s (Dirac) | ~0.70* |
| 26 | Fe | 4s      | 0.90             | 92 | U  | 7s (Dirac) | 0.99   |
| 29 | Cu | 4s      | 0.96             |    |    |            |        |

\* Z\u226555 tables are RELATIVISTIC (Dirac); Clementi-Raimondi is
nonrelativistic, so the s/p ratios carry a systematic relativistic-vs-NR
offset (~0.7 for the 5d/6s block) -- the contraction itself is validated
against the NIST RLDA eigenvalues. Pd (Z=46, 0.33) is a documented
X\u03b1/LDA d-shell limitation (4d eigenvalue matches NIST LDA).

(Old hydrogenic model: Li 1.30, Na 1.45, Fe 1.54, Kr 1.52, Cs 3.34,
Au 3.40, U 4.96.)
