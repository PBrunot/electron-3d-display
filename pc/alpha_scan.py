#!/usr/bin/env python3
"""Quick alpha scan: solve representative elements at several exchange
factors and compare the valence-subshell mode of r^2 R^2 against the
in-repo Clementi-Raimondi radii table."""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'micropython'))

import micropython_shim  # noqa
import slater  # noqa
import clementi_radii  # noqa

from hfs_solver import make_grid, solve_element, radial_mode_from_u, valence_subshell

PM_PER_BOHR = 52.9177210903
ELEMENTS = [1, 2, 3, 6, 10, 11, 18, 26, 36, 54, 55, 79, 82, 92]
ALPHAS = [1.0, 0.85, 2.0 / 3.0]


def main():
    grid = make_grid()
    for alpha in ALPHAS:
        print("=== alpha = %.4f ===" % alpha)
        print(" Z  el  val  mode(pm)  lit(pm)  ratio")
        for z in ELEMENTS:
            res = solve_element(z, alpha=alpha, grid=grid, max_iter=100)
            n, ell = valence_subshell(res['config'])
            u = dict(((s[0], s[1]), s) for s in res['states'])[(n, ell)][4]
            mode_pm = radial_mode_from_u(u, grid[0]) * PM_PER_BOHR
            lit = clementi_radii.CLEMENTI_RADIUS_PM.get(z)
            ratio = mode_pm / lit if lit else float('nan')
            print("%3d %-2s  %d%s  %7.1f  %6.0f  %5.2f"
                  % (z, slater.element_symbol(z), n, 'spdf'[ell], mode_pm,
                     lit if lit else -1, ratio))
        print()


if __name__ == '__main__':
    main()
