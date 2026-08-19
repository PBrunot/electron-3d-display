#!/usr/bin/env python3
"""Per-subshell eigenvalue cross-check: old HFS tables vs SPARC-atomSFE
(NIST-exact LDA) tables. Shows how close the old solver's eigenvalues are
to the library's NIST-validated ones."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import numpy as np

import hfs_tables as ht

OLD = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hfs_tables - Copia.npz')
NEW = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hfs_tables_atomsfe.npz')


def main():
    old = ht.load(OLD)
    new = ht.load(NEW)
    print("old coverage: %d elements (min %d, max %d)" % (
        len(old.z_list), min(old.z_list), max(old.z_list)))
    print("new coverage: %d elements (min %d, max %d)" % (
        len(new.z_list), min(new.z_list), max(new.z_list)))

    # Valence subshell (highest-l among highest-n) eigenvalue difference --
    # the physically meaningful comparison (core states differ by model:
    # exchange-only HFS vs LDA, relativistic vs nonrelativistic).
    val_diffs = []
    for z in sorted(set(old.z_list) & set(new.z_list)):
        nmax = max(n for n, _e, _o in old.config(z))
        val = max(((n, ell) for n, ell, o in old.config(z) if n == nmax),
                  key=lambda t: t[1])
        if not new.has(z, *val):
            continue
        eo = old.source(z, *val).energy
        en = new.source(z, *val).energy
        val_diffs.append((abs(eo - en) * 27.211386245988, z, val, eo, en))
    val_diffs.sort(reverse=True)
    print("\nValence |dE| (old HFS vs atomSFE LDA), worst first:")
    for dE, z, (n, ell), eo, en in val_diffs[:12]:
        print("  Z=%3d %-2s %d%s: old %9.4f  atomsfe %9.4f  |dE| %7.4f eV" % (
            z, _sym(z), n, 'spdf'[ell], eo, en, dE))
    med = float(np.median([d for d, *_ in val_diffs]))
    print("  ... median |dE| over %d elements = %.4f eV" % (len(val_diffs), med))
    old.close()
    new.close()


_SYMS = ('H He Li Be B C N O F Ne Na Mg Al Si P S Cl Ar K Ca Sc Ti V Cr Mn Fe '
         'Co Ni Cu Zn Ga Ge As Se Br Kr Rb Sr Y Zr Nb Mo Tc Ru Rh Pd Ag Cd In '
         'Sn Sb Te I Xe Cs Ba La Ce Pr Nd Pm Sm Eu Gd Tb Dy Ho Er Tm Yb Lu Hf '
         'Ta W Re Os Ir Pt Au Hg Tl Pb Bi Po At Rn Fr Ra Ac Th Pa U').split()


def _sym(z):
    return _SYMS[z - 1] if 1 <= z <= len(_SYMS) else '?'


if __name__ == '__main__':
    sys.exit(main())
