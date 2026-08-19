#!/usr/bin/env python3
"""Compare the solver's eigenvalues against the NIST reference data
(dftdata archive from math.nist.gov/DFTdata/atomdata/).

Two comparisons:

1. RLDA spin-orbit splits vs the Dirac solver: for selected elements, run
   the HFS SCF then the Dirac pass (pc/hfs_solver.solve_element_relativistic)
   and compare, per occupied (nl) with l>0:
       split_NIST = eps(nlM) - eps(nlP)
       split_ours = eps(kappa=+l) - eps(kappa=-(l+1))
   Spin-orbit is central-field physics, nearly correlation-free, so this is
   a direct test of the relativistic machinery. Also compares the absolute
   P/M eigenvalues.

2. LDA valence eigenvalues vs the HFS eigenvalues at a given alpha: the
   NIST LDA includes correlation (ours is exchange-only), so the pattern
   must track while absolute values differ by ~10-20%; the closest alpha
   wins.

Usage:
    python3 pc/nist_compare.py <dftdata-root> [--alpha 1.0]
                                [--elements 18,36,54,79,92]
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'micropython'))

import micropython_shim  # noqa
import slater  # noqa

import numpy as np  # noqa: E402

from hfs_solver import make_grid, solve_element, solve_element_relativistic  # noqa: E402
from dirac_solver import solve_dirac_state  # noqa: E402

HARTREE_EV = 27.211386245988


def parse_element_file(path):
    """Parse one NIST element data file -> (energies dict, {orbital: eps})."""
    energies = {}
    orbitals = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if parts[0] in ('Etot', 'Ekin', 'Ecoul', 'Eenuc', 'Exc'):
                energies[parts[0]] = float(parts[2])
            else:
                try:
                    orbitals[parts[0]] = float(parts[1])
                except (IndexError, ValueError):
                    pass
    return energies, orbitals


# Noble-gas cores used by the NIST configurations file notation [X].
_NOBLE_GAS_CORES = {
    'He': [(1, 0, 2)],
    'Ne': [(1, 0, 2), (2, 0, 2), (2, 1, 6)],
    'Ar': [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6)],
    'Kr': [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6),
           (3, 2, 10), (4, 0, 2), (4, 1, 6)],
    'Xe': [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6),
           (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (5, 0, 2), (5, 1, 6)],
    'Rn': [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6),
           (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14),
           (5, 0, 2), (5, 1, 6), (5, 2, 10), (6, 0, 2), (6, 1, 6)],
}


def parse_configurations(path):
    """Parse the NIST configurations file (neutral + first-cation columns,
    e.g. "26 Fe [Ar] 3d^6 4s^2  [Ar] 3d^6 4s^1") -> {Z: [(n, l, occ), ...]}
    for the NEUTRAL species. The columns are whitespace-delimited with no
    explicit separator, so the neutral config is reconstructed greedily:
    expand [X] cores, accumulate shells until the running electron count
    reaches Z (the cation then has Z-1 in the remaining tokens)."""
    configs = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                z = int(parts[0])
            except ValueError:
                continue
            shells = []
            running = 0
            for tok in parts[2:]:
                if tok.startswith('['):
                    core = _NOBLE_GAS_CORES.get(tok.strip('[]'))
                    if core:
                        for n, l, o in core:
                            shells.append((n, l, o))
                            running += o
                    continue
                if '^' not in tok:
                    continue
                i = 0
                while i < len(tok) and tok[i].isdigit():
                    i += 1
                n = int(tok[:i])
                l = 'spdf'.index(tok[i]) if i < len(tok) and tok[i] in 'spdf' else -1
                occ = int(tok.split('^')[1])
                if l >= 0 and occ > 0:
                    shells.append((n, l, occ))
                    running += occ
                if running >= z:
                    break  # neutral column complete
            configs[z] = shells
    return configs


def compare_configurations(root):
    """Cross-check slater.electron_configuration() against the NIST
    configurations file (neutral species)."""
    path = os.path.join(root, 'configurations')
    if not os.path.isfile(path):
        print("configurations file not found under %s" % root)
        return
    nist = parse_configurations(path)
    bad = 0
    for z in sorted(nist):
        ours = slater.electron_configuration(z)
        # canonicalize: sort by (n, l) both sides
        theirs = sorted(nist[z])
        ours_s = sorted(ours)
        if theirs != ours_s:
            bad += 1
            print("  Z=%3d %-2s: NIST %s vs ours %s"
                  % (z, slater.element_symbol(z),
                     " ".join("%d%s%d" % (n, 'spdf'[l], o) for n, l, o in theirs),
                     " ".join("%d%s%d" % (n, 'spdf'[l], o) for n, l, o in ours_s)))
    total = len(nist)
    print("configurations: %d/%d elements match NIST%s"
          % (total - bad, total, "" if bad == 0 else " (%d differ)" % bad))


def valence_label(z):
    config = slater.electron_configuration(z)
    n, ell = max(((nn, ll) for nn, ll, o in config
                  if nn == max(x[0] for x in config)), key=lambda t: t[1])
    return n, ell, "%d%s" % (n, 'spdf'[ell])


def compare_spin_orbit(root, elements, alpha=1.0):
    rlda_dir = os.path.join(root, 'RLDA', 'neutrals')
    grid = make_grid()
    print("=== RLDA spin-orbit splits: NIST vs our Dirac solver "
          "(potential at alpha=%.3f) ===" % alpha)
    print("(eps(M) - eps(P), Hartree; M = j=l-1/2, P = j=l+1/2)")
    for z in elements:
        fname = "%02d%s" % (z, slater.element_symbol(z))
        path = os.path.join(rlda_dir, fname)
        if not os.path.isfile(path):
            print("Z=%d: RLDA file missing" % z)
            continue
        _energies, orbs = parse_element_file(path)
        res = solve_element_relativistic(z, alpha=alpha, grid=grid)
        print("-- %s (Z=%d)" % (slater.element_symbol(z), z))
        for n, ell, occ, E, u in res['states']:
            if ell == 0:
                continue
            label_p = "%d%sP" % (n, 'spdf'[ell])
            label_m = "%d%sM" % (n, 'spdf'[ell])
            if label_p not in orbs or label_m not in orbs:
                continue
            split_nist = orbs[label_m] - orbs[label_p]
            # our Dirac eps for the two j values (recomputed on the
            # converged potential; the merged state only keeps the average)
            r, dt = grid
            t = np.log(r / r[0])
            q = res['q']
            from hfs_solver import potential_from_q
            V = potential_from_q(z, r, q, alpha=alpha, latter=True)
            eps_p = solve_dirac_state(V, r, t, dt, -(ell + 1), n, z)[0]
            eps_m = solve_dirac_state(V, r, t, dt, ell, n, z)[0]
            split_ours = eps_m - eps_p
            print("  %d%s: NIST split %7.4f  ours %7.4f  (ratio %.2f)"
                  % (n, 'spdf'[ell], split_nist, split_ours,
                     split_ours / split_nist if split_nist else float('nan')))


def compare_lda_valence(root, alpha, elements):
    lda_dir = os.path.join(root, 'LDA', 'neutrals')
    grid = make_grid()
    print("=== LDA valence eigenvalues: NIST (with correlation) vs our HFS (exchange-only, alpha=%.3f) ==="
          % alpha)
    for z in elements:
        fname = "%02d%s" % (z, slater.element_symbol(z))
        path = os.path.join(lda_dir, fname)
        if not os.path.isfile(path):
            print("Z=%d: LDA file missing" % z)
            continue
        _energies, orbs = parse_element_file(path)
        res = solve_element(z, alpha=alpha, grid=grid)
        n, ell, label = valence_label(z)
        ours = dict(((s[0], s[1]), s[3]) for s in res['states'])[(n, ell)]
        nist = orbs.get(label)
        if nist is None:
            print("Z=%d: valence label %s not in NIST file" % (z, label))
            continue
        print("  Z=%3d %-2s %s: NIST %9.4f  ours %9.4f Ha  (ours %+6.2f eV vs NIST)"
              % (z, slater.element_symbol(z), label, nist, ours,
                 (ours - nist) * HARTREE_EV))


def main(argv):
    root = argv[0] if argv else '.'
    rest = argv[1:]
    alpha = 1.0
    elements = [18, 30, 36, 54, 79, 82, 92]
    if '--alpha' in rest:
        alpha = float(rest[rest.index('--alpha') + 1])
    if '--elements' in rest:
        elements = [int(x) for x in rest[rest.index('--elements') + 1].split(',')]
    compare_spin_orbit(root, elements, alpha)
    compare_lda_valence(root, alpha, elements)
    compare_configurations(root)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
