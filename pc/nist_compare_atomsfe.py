#!/usr/bin/env python3
"""Compare SPARC-atomSFE table eigenvalues against the NIST dftdata LDA
reference (archive under examples/"nis data"/dftdata, from
math.nist.gov/DFTdata/atomdata/).

SPARC-atomSFE with LDA_SVWN is the same (nonrelativistic) LDA the NIST
archive tabulates, so the eigenvalues should agree to ~1e-5 Ha -- this is
the external validation that replaces pc/nist_compare.py's live HFS/Dirac
comparisons for the new tables.

Usage:
    python3 pc/nist_compare_atomsfe.py \
        "examples/nis data/dftdata" [--tables pc/hfs_tables_atomsfe.npz]
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def parse_element_file(path):
    """NIST dftdata element file -> {orbital_label: eigenvalue (Ha)}."""
    orbitals = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2 and parts[0] not in ('Etot', 'Ekin', 'Ecoul',
                                                    'Eenuc', 'Exc'):
                try:
                    orbitals[parts[0]] = float(parts[1])
                except ValueError:
                    pass
    return orbitals


def label(n, ell):
    return "%d%s" % (n, 'spdf'[ell])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('root', help='dftdata archive root')
    ap.add_argument('--tables', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'hfs_tables_atomsfe.npz'))
    ap.add_argument('--zmin', type=int, default=1)
    ap.add_argument('--zmax', type=int, default=92)
    ap.add_argument('--tol', type=float, default=2e-5,
                    help='max |dE| (Ha) per subshell before flagging')
    args = ap.parse_args(argv)

    import hfs_tables as ht
    tabs = ht.load(args.tables)

    n_sub = n_elem = 0
    worst = 0.0
    worst_where = None
    n_fail = 0
    for z in range(args.zmin, args.zmax + 1):
        path = os.path.join(args.root, 'LDA', 'neutrals', '%02d%s' % (
            z, _sym(z)))
        if not os.path.isfile(path):
            print("Z=%d: NIST file missing, skipped" % z)
            continue
        nist = parse_element_file(path)
        if not nist:
            print("Z=%d: NIST file empty, skipped" % z)
            continue
        n_elem += 1
        dmax = 0.0
        dmax_where = None
        for n, ell, occ in tabs.config(z):
            lab = label(n, ell)
            if lab not in nist:
                continue
            dE = abs(tabs.source(z, n, ell).energy - nist[lab])
            n_sub += 1
            if dE > dmax:
                dmax, dmax_where = dE, lab
            if dE > worst:
                worst, worst_where = dE, (z, lab)
            if dE > args.tol:
                n_fail += 1
        print("Z=%3d %-2s  max|dE| over %d subshells = %.3e Ha (%s)" % (
            z, _sym(z), len(tabs.config(z)), dmax, dmax_where or '-'))
    tabs.close()

    print("\n%d elements, %d subshells compared" % (n_elem, n_sub))
    print("worst |dE| = %.3e Ha at %s" % (worst, worst_where))
    print("subshells exceeding %.1e Ha: %d" % (args.tol, n_fail))
    return 0 if n_fail == 0 else 1


_SYMS = ('H He Li Be B C N O F Ne Na Mg Al Si P S Cl Ar K Ca Sc Ti V Cr Mn Fe '
         'Co Ni Cu Zn Ga Ge As Se Br Kr Rb Sr Y Zr Nb Mo Tc Ru Rh Pd Ag Cd In '
         'Sn Sb Te I Xe Cs Ba La Ce Pr Nd Pm Sm Eu Gd Tb Dy Ho Er Tm Yb Lu Hf '
         'Ta W Re Os Ir Pt Au Hg Tl Pb Bi Po At Rn Fr Ra Ac Th Pa U').split()


def _sym(z):
    return _SYMS[z - 1] if 1 <= z <= len(_SYMS) else '?'


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
