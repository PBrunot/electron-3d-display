#!/usr/bin/env python3
"""Generate the radial atom tables with SPARC-atomSFE (offline, PC-only).

Replaces pc/hfs_solver.py's hand-rolled HFS/Dirac solver as the source of
the radial tables consumed by the atom viewers: every occupied (n, ell)
subshell of Z=1..92 is solved as an all-electron Kohn-Sham state with the
SPARC-atomSFE spectral-finite-element code (examples/SPARC-atomSFE, pip-
installed into pc/_atomsfe_vendor), LDA_SVWN exchange-correlation by
default. The package reproduces the NIST dftdata LDA eigenvalues to all
printed digits (verified on Fe/U), so these tables inherit that external
validation.

The output npz has EXACTLY the pc/hfs_tables.py schema, so every consumer
(hfs_tables.load(), atom_view_pc, screenshot.py, validate_atoms.py) works
unchanged:

    r            float64[2001]  shared log-uniform grid (Bohr)
    z_list       int32[Z]       atomic numbers present
    z<N>_config  int32[(k,3)]   (n, ell, occ) configuration (atomSFE order)
    z<N>_<n>_<ell>_u   float32[2001]  u(r) = r*R(r), normalized (int u^2 dr = 1)
    z<N>_<n>_<ell>_E   float64         eigenvalue (Hartree)
    z<N>_<n>_<ell>_occ int32          occupancy

Scope: Z=1..92 ONLY (the library's hard cap; the display is deliberately
limited to that range). `--merge-old` can still graft Z outside [zmin,zmax]
from an existing npz, but the default tables are the pure atomSFE set.

Notes / known limitations (see pc/RUN_HFS.md for the old model's story):
  * The library is non-relativistic: the one-shot radial Dirac pass the old
    solver applied for Z>=55 (s/p contraction, U 7s 242->174 pm) is NOT
    reproduced here. Z>=55 tables are plain LDA (Schrodinger) results.
  * LDA valence orbitals are more diffuse than the HF-based
    Clementi-Raimondi reference (self-interaction error); pc/atom_view_pc.py
    applies a per-element CR size calibration so the display sizes match
    literature.

Usage:
    python pc/hfs_atomsfe.py --zmin 1 --zmax 92 --out pc/hfs_tables.npz
    python pc/hfs_atomsfe.py --zmin 26 --zmax 26   # single element
"""

import argparse
import math
import os
import sys

import numpy as np

VENDOR = os.path.join(os.path.dirname(os.path.abspath(__file__)), '_atomsfe_vendor')
sys.path.insert(0, VENDOR)

PM_PER_BOHR = 52.9177210903
GRID_R0 = 1e-6
GRID_RMAX = 100.0
GRID_N = 2001


def make_grid():
    """The shared output grid -- identical to hfs_solver.make_grid() default."""
    t = np.linspace(0.0, math.log(GRID_RMAX / GRID_R0), GRID_N)
    return GRID_R0 * np.exp(t)


def solve_element(z, functional, domain, fe, order, quad, tol, verbose):
    """One all-electron LDA solve for element z; returns (config, states).

    config: list of (n, ell, occ) in the solver's occupation order.
    states: list of (n, ell, occ, E, u_out) with u_out = u(r) = r*R(r) on the
    shared output grid, normalized so int u^2 dr = 1 and u[0] > 0.
    """
    from atom.solver import AtomicDFTSolver

    solver = AtomicDFTSolver(
        atomic_number=z,
        xc_functional=functional,
        domain_size=float(domain),
        finite_element_number=int(fe),
        polynomial_order=int(order),
        quadrature_point_number=int(quad),
        mesh_type='exponential',
        mesh_concentration=101.0,
        scf_tolerance=float(tol),
        verbose=bool(verbose),
        all_electron_flag=True,
        use_oep=False,
        use_preconditioner=True,
    )
    res = solver.solve()
    if not res.get('converged'):
        raise RuntimeError("Z=%d: SCF did not converge" % z)
    occ = res['occupation_info']
    rq = np.asarray(res['quadrature_nodes'], dtype=np.float64)
    orb = np.asarray(res['orbitals'], dtype=np.float64)  # (n_quad, n_states)
    E_all = np.asarray(res['eigen_energies'], dtype=np.float64)
    r_out = make_grid()

    config = []
    states = []
    for i in range(occ.n_states):
        n = int(occ.occ_n[i])
        ell = int(occ.occ_l[i])
        o = float(occ.occ_spin_up_plus_spin_down[i])
        E = float(E_all[i])
        # u = r*R on the source quadrature grid, then onto the output log grid.
        # np.interp clamps to the boundary values; beyond the domain the bound
        # state is ~0, which is what the clamp gives.
        u_src = rq * orb[:, i]
        u_out = np.interp(r_out, rq, u_src)
        # Renormalize on the output grid (the FE orbitals are L2-normalized in
        # the FE sense; this makes int u^2 dr = 1 exactly as the schema says).
        norm = np.trapezoid(u_out * u_out, r_out)
        if norm <= 0.0 or not np.isfinite(norm):
            raise RuntimeError("Z=%d %d%c: bad normalization %g" % (z, n, 'spdf'[ell], norm))
        u_out = u_out / math.sqrt(norm)
        if u_out[0] < 0.0:
            u_out = -u_out
        config.append((n, ell, o))
        states.append((n, ell, o, E, u_out))
    return config, states


def save_tables(results, out_path, merge_old=None):
    """Write (or merge-and-write) the npz. results: {z: (config, states)}.

    Merge semantics: elements from merge_old outside `results` are kept as-is
    (u arrays copied verbatim); elements solved here override same-Z entries.
    """
    r_out = make_grid()
    arrays = {'r': r_out.astype(np.float64)}
    by_z = {}  # z -> (config, states); states as (n, ell, occ, E, u)

    if merge_old:
        data = np.load(merge_old)
        old_r = data['r']
        if not np.allclose(old_r, r_out):
            raise SystemExit("merge source grid %s..%s differs from output "
                             "grid %s..%s" % (old_r[0], old_r[-1], r_out[0], r_out[-1]))
        for oz in (int(v) for v in data['z_list']):
            if oz in by_z:
                continue
            config = [tuple(int(v) for v in row) for row in data['z%d_config' % oz]]
            states = []
            for n, ell, occn in config:
                states.append((n, ell, occn,
                               float(data['z%d_%d_%d_E' % (oz, n, ell)]),
                               np.asarray(data['z%d_%d_%d_u' % (oz, n, ell)],
                                          dtype=np.float64)))
            by_z[oz] = (config, states)
        data.close()

    for z in sorted(results):
        by_z[z] = results[z]

    for z in sorted(by_z):
        config, states = by_z[z]
        arrays['z%d_config' % z] = np.array(config, dtype=np.int32)
        for n, ell, occn, E, u in states:
            key = 'z%d_%d_%d' % (z, n, ell)
            arrays[key + '_u'] = u.astype(np.float32)
            arrays[key + '_E'] = np.float64(E)
            arrays[key + '_occ'] = np.int32(occn)
    arrays['z_list'] = np.array(sorted(by_z), dtype=np.int32)
    n_sub = sum(len(by_z[z][1]) for z in by_z)
    np.savez(out_path, **arrays)
    print("wrote %s (%d elements, %d subshells total)" % (
        out_path, len(by_z), n_sub))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zmin', type=int, default=1)
    ap.add_argument('--zmax', type=int, default=92,
                    help='SPARC-atomSFE supports 1..92 (hard cap in its '
                         'occupation tables)')
    ap.add_argument('--functional', default='LDA_SVWN',
                    help='XC functional (LDA_SVWN reproduces the NIST dftdata '
                         'LDA eigenvalues to printed precision)')
    ap.add_argument('--domain', type=float, default=40.0)
    ap.add_argument('--fe', type=int, default=8, help='finite elements')
    ap.add_argument('--order', type=int, default=24, help='polynomial order')
    ap.add_argument('--quad', type=int, default=72, help='quadrature points')
    ap.add_argument('--tol', type=float, default=1e-9, help='SCF tolerance')
    ap.add_argument('--out', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'hfs_tables_atomsfe.npz'))
    ap.add_argument('--merge-old', default=None, metavar='NPZ',
                    help='keep Z outside [zmin,zmax] from an existing npz '
                         '(e.g. the old HFS tables, for Z=93..118)')
    ap.add_argument('--jobs', type=int, default=1,
                    help='parallel elements (multiprocessing; 0 = all cores)')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args(argv)

    z_range = list(range(args.zmin, args.zmax + 1))
    if any(z < 1 or z > 92 for z in z_range):
        raise SystemExit("SPARC-atomSFE only covers Z=1..92 (use --merge-old "
                         "to keep the rest from an existing npz)")

    def run(z):
        import time as _time
        t0 = _time.time()
        config, states = solve_element(z, args.functional, args.domain, args.fe,
                                       args.order, args.quad, args.tol,
                                       args.verbose)
        dt = _time.time() - t0
        n, ell = max((s[0], s[1]) for s in states)  # crude valence tag
        u = dict(((s[0], s[1]), s) for s in states)[(n, ell)][4]
        mode = _mode_radius(u, make_grid())
        print("Z=%3d %-2s  %6.1f s | valence %d%s mode %6.1f pm" % (
            z, _sym(z), dt, n, 'spdf'[ell], mode * PM_PER_BOHR), flush=True)
        return z, (config, states)

    results = {}
    if args.jobs == 0 or args.jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        workers = os.cpu_count() if args.jobs == 0 else args.jobs
        with ProcessPoolExecutor(max_workers=workers) as ex:
            for z, res in ex.map(run, z_range):
                results[z] = res
    else:
        for z in z_range:
            z, res = run(z)
            results[z] = res
    save_tables(results, args.out, merge_old=args.merge_old)
    return 0


def _mode_radius(u, r):
    """Parabolic-refined mode of u^2 = r^2 R^2 (same as hfs_solver)."""
    w = u * u
    i = int(np.argmax(w))
    if 0 < i < len(w) - 1:
        x0, x1, x2 = r[i - 1], r[i], r[i + 1]
        y0, y1, y2 = w[i - 1], w[i], w[i + 1]
        denom = (x0 - x1) * (x0 - x2) * (x1 - x2)
        a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / denom
        b = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / denom
        if abs(a) > 1e-30:
            peak = -b / (2.0 * a)
            if r[i - 1] < peak < r[i + 1]:
                return peak
    return float(r[i])


_SYMS = ('H He Li Be B C N O F Ne Na Mg Al Si P S Cl Ar K Ca Sc Ti V Cr Mn Fe '
         'Co Ni Cu Zn Ga Ge As Se Br Kr Rb Sr Y Zr Nb Mo Tc Ru Rh Pd Ag Cd In '
         'Sn Sb Te I Xe Cs Ba La Ce Pr Nd Pm Sm Eu Gd Tb Dy Ho Er Tm Yb Lu Hf '
         'Ta W Re Os Ir Pt Au Hg Tl Pb Bi Po At Rn Fr Ra Ac Th Pa U Np Pu Am '
         'Cm Bk Cf Es Fm Md No Lr Rf Db Sg Bh Hs Mt Ds Rg Cn Nh Fl Mc Lv Ts '
         'Og').split()


def _sym(z):
    return _SYMS[z - 1] if 1 <= z <= len(_SYMS) else '?'


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
