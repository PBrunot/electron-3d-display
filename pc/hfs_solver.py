#!/usr/bin/env python3
"""Hartree-Fock-Slater screened-potential atomic solver (offline, PC-only).

Solves every occupied (n, ell) subshell's radial wavefunction of a neutral
atom as an eigenstate of a single central potential built self-consistently
from its own electron density -- the classic central-field approximation
(Herman & Skillman 1963, see pc/screened_potential_model.md for the full
design and references). Replaces the hydrogenic "one Z_eff per subshell"
approximation of micropython/slater.py for the high-Z atom model:

  V(r) = -Z/r + V_ee(r) + V_x(r)
  V_ee: Coulomb potential of the spherically averaged electron density
  V_x : local (Slater) exchange, -3*alpha*(3*rho/(8*pi))^(1/3)
  Latter cutoff: V := min(V, -1/r)  -- restores the physical -1/r tail for
      the outermost electron (removes the Hartree self-interaction error at
      infinity, Latter 1955)

Radial equation (Hartree atomic units, r in Bohr radii), u(r) = r*R(r):

  u'' = [l(l+1)/r^2 + 2(V - E)] u

On a log-uniform grid r = r0*exp(t) with v(t) = r^(1/2)*R(r) this becomes a
clean 1D Schrodinger form without first-derivative term,

  v'' = [l(l+1) + 1/4 + 2(V - E) r^2] v,

i.e. a symmetric tridiagonal generalized eigenproblem A v = E B v
(B = diag(2 r^2)), solved with ARPACK shift-invert
(scipy.sparse.linalg.eigsh) -- see solve_states_l() for why the tridiagonal
LAPACK paths are not used here. Eigenfunctions of the SAME potential with
different n are automatically orthogonal, which reproduces the
core-orthogonality contraction that the hydrogenic model misses.

Numpy/scipy are fine here: this is an offline table generator (like
tools/splash_gen), NOT code that runs on the device or in the shared
micropython/ samplers -- its output feeds those samplers via tabulated
radial functions (see pc/hfs_tables.py).

Usage:
    python3 pc/hfs_solver.py --zmin 1 --zmax 118 --out pc/hfs_tables.npz
    python3 pc/hfs_solver.py --zmin 92 --zmax 92          # single element
    python3 pc/hfs_solver.py --coulomb-check              # machinery gate:
        solve hydrogenic states in the exact -Z/r potential and compare
        E_n and mode radii against the analytic values
"""

import argparse
import math
import os
import sys

import numpy as np
from scipy.sparse import diags
from scipy.sparse.linalg import eigsh

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'micropython'))

import micropython_shim  # noqa: F401 -- CPython shim for @micropython.native
import orbitals  # noqa: E402
import slater  # noqa: E402

from dirac_solver import solve_dirac_state  # noqa: E402 -- relativistic option

# Hartree <-> eV (CODATA 2018).
HARTREE_EV = 27.211386245988
# Bohr radius in pm (matches micropython/cloud_common.py's PM_PER_BOHR).
PM_PER_BOHR = 52.9177210903

# Default grid: log-uniform, r0..rmax, N points. r0 must be well inside the
# innermost orbital of the heaviest element (U 1s mode ~ 1/92 a0; r0=1e-5
# left a ~0.4% energy error on U's 1s, r0=1e-6 reduces it to ~0.04%);
# rmax must cover the most diffuse valence tail (Fr/Ra 7s). N=2000 gives
# ~0.7% relative step everywhere (constant on a log grid) -- verified
# equivalent to N=3000 for the states this model needs.
DEFAULT_R0 = 1e-6
DEFAULT_RMAX = 100.0
DEFAULT_N = 1500


def make_grid(r0=DEFAULT_R0, rmax=DEFAULT_RMAX, n=DEFAULT_N):
    """Log-uniform radial grid: r_i = r0*exp(t_i), t uniform in [0, ln(rmax/r0)].
    Returns (r, dt) with dt the uniform step in t."""
    t = np.linspace(0.0, math.log(rmax / r0), n)
    return r0 * np.exp(t), t[1] - t[0]


def hydrogenic_u(z_eff, n, ell, r):
    """Normalized hydrogenic u(r) = r*R_{n,ell}(z_eff*r) -- the current model's
    radial function (same substitution slater.z_eff_radial() uses), used as
    the SCF initial guess. Normalized so int u^2 dr = 1."""
    coeff = orbitals.laguerre_coeffs(n, ell)
    x = z_eff * r
    # R(x) = (sum_k coeff[k] x^k) * x^ell * exp(-x/n), Horner-style
    poly = np.full_like(r, coeff[n - ell - 1])
    for k in range(n - ell - 2, -1, -1):
        poly = poly * x + coeff[k]
    R = poly * x ** ell * np.exp(-x / n)
    u = r * R
    norm = np.trapezoid(u * u, r)
    if norm <= 0.0 or not np.isfinite(norm):
        raise RuntimeError("bad hydrogenic normalization n=%d ell=%d z_eff=%g" % (n, ell, z_eff))
    u /= math.sqrt(norm)
    if u[0] < 0.0:
        u *= -1.0
    return u


def cumtrapz(y, x):
    """Cumulative trapezoid integral int_{x0}^{x_i} y dx at every grid point
    (exact for linearly interpolated y)."""
    return np.concatenate(([0.0], np.cumsum(0.5 * (y[1:] + y[:-1]) * np.diff(x))))


def q_from_orbitals(us, occs, r):
    """Radial charge line density q(r) = 4*pi*r*rho(r) = sum occ * u^2 / r, the
    quantity whose cumulative integral counts electrons (int q dr = N)."""
    q = np.zeros_like(r)
    for u, occ in zip(us, occs):
        q += occ * u * u / r
    return q


def potential_from_q(z, r, q, alpha=1.0, latter=True):
    """Central potential V(r) from the charge line density q (see
    q_from_orbitals): nuclear + electron-electron + Slater exchange, with the
    Latter -1/r cutoff. Positive V_ee (repulsion), negative V_x (exchange).

    Spherical-cloud Coulomb potential (q = 4*pi*r*rho, so 4*pi*s^2*rho =
    q*s and 4*pi*s*rho = q):
      V_ee(r) = (1/r) int_0^r q(s) s ds  +  int_r^inf q(s) ds
    (first term: shell charge inside r acting as a point at the origin;
     second: charge outside r acting as a shell at r)."""
    A = cumtrapz(q * r, r)     # int_0^r q(s) s ds
    B = cumtrapz(q, r)         # int_0^r q(s) ds  (= electrons inside r)
    n_e = B[-1]                # total electrons (int_0^inf q ds)
    vee = A / r + (n_e - B)
    # volume density rho = q / (4 pi r^2)
    rho = q / (4.0 * math.pi * r * r)
    vx = -3.0 * alpha * (3.0 * rho / (8.0 * math.pi)) ** (1.0 / 3.0)
    v = -z / r + vee + vx
    if latter:
        v = np.minimum(v, -1.0 / r)
    return v


def solve_states_l(V, r, dt, ell, n_states, z=1, sigma=None):
    """The n_states lowest eigenstates of angular momentum ell in potential V.

    Returns (E, u, interior): E[n_states] ascending, u (n_interior, n_states)
    normalized (int u^2 dr = 1, u[0] > 0), interior = index array mapping u's
    rows onto the full grid (grid point 0 and N-1 are the Dirichlet
    boundaries where u = 0).

    Numerics: the log-grid discretization gives a symmetric tridiagonal
    generalized eigenproblem A v = E B v (B = diag(2 r^2)) whose dynamic
    range is ~1e16 (entries ~1e16 near r0 for heavy Z down to ~10 far out).
    LAPACK's dstevx/dsterf lose the SHALLOW eigenvalues in that range
    (verified: dsterf misplaces 4d of hydrogen by 4% at r0=1e-6), so this
    uses ARPACK shift-invert (scipy.sparse.linalg.eigsh) with a shift below
    the deepest possible bound -- the k eigenvalues nearest the shift are
    exactly the k occupied states of this l, and shift-invert stays
    well-conditioned regardless of the matrix norm. `sigma` may warm-start
    the shift from the previous SCF iteration's deepest eigenvalue.
    """
    N = len(r)
    interior = np.arange(1, N - 1)
    ri = r[interior]
    r2 = ri * ri
    q0 = ell * (ell + 1) + 0.25 + 2.0 * V[interior] * r2
    b = 2.0 * r2  # B = diag(2 r^2)
    m = len(ri)
    o = -1.0 / dt ** 2 * np.ones(m - 1)
    A = diags([o, 2.0 / dt ** 2 + q0, o], [-1, 0, 1], format='csr')
    B = diags(b, 0, format='csr')
    if sigma is None:
        # shift strictly below the deepest possible bound of this potential:
        # the bare-nucleus hydrogenic ground state -z^2/2 (the SCF potential
        # with the Latter cutoff is never deeper than that).
        sigma = -0.5 * z * z - 2.0
    E, v = eigsh(A, k=n_states, M=B, sigma=sigma, which='LM')
    order = np.argsort(E)
    E = E[order]
    v = v[:, order]
    u = np.sqrt(ri)[:, None] * v  # u = r^(1/2) v = r R
    for j in range(n_states):
        uj = u[:, j]
        norm = np.trapezoid(uj * uj, ri)
        u[:, j] = uj / math.sqrt(norm)
        if u[0, j] < 0.0:
            u[:, j] *= -1.0
    return E, u, interior


def occupied_by_l(config):
    """config -> {ell: [(n, occ), ...]} in ascending n (see solve_element for
    why ascending n maps 1:1 onto ascending energy for a fixed ell)."""
    out = {}
    for n, ell, occ in config:
        out.setdefault(ell, []).append((n, occ))
    for ell in out:
        out[ell].sort()
    return out


def solve_element(z, alpha=1.0, max_iter=80, mix=0.35, e_tol=1e-7, q_tol=1e-5,
                  grid=None, verbose=False):
    """Self-consistent HFS solution for neutral element z.

    Returns a dict with the converged states:
        {'z': z, 'config': config, 'r': r, 'states': [(n, ell, occ, E, u), ...],
         'q': q, 'iterations': it}
    u is on the FULL grid (len(r), zero at the Dirichlet boundaries); r is
    the full grid.
    """
    r, dt = grid if grid is not None else make_grid()
    config = slater.electron_configuration(z)
    by_l = occupied_by_l(config)

    # Initial guess: the current model's hydrogenic radial functions.
    us, occs, keys = [], [], []
    for ell, lst in by_l.items():
        for n, occ in lst:
            z_eff = slater.z_eff_radial(z, config, n, ell)
            us.append(hydrogenic_u(z_eff, n, ell, r))
            occs.append(occ)
            keys.append((n, ell))
    q = q_from_orbitals(us, occs, r)

    E_prev = None
    sigma_by_l = {}  # warm-start shifts: previous iteration's deepest E per l
    q_prev = q.copy()
    it = 0
    for it in range(1, max_iter + 1):
        V = potential_from_q(z, r, q, alpha=alpha, latter=True)
        states = []
        E_all = []
        for ell, lst in by_l.items():
            sigma = sigma_by_l.get(ell)
            E, U, interior = solve_states_l(V, r, dt, ell, len(lst), z=z, sigma=sigma)
            sigma_by_l[ell] = float(np.min(E)) - 0.05
            for j, (n, occ) in enumerate(lst):
                # full-grid u (u=0 at the Dirichlet boundaries)
                u_full = np.zeros_like(r)
                u_full[interior] = U[:, j]
                states.append((n, ell, occ, E[j], u_full))
                E_all.append(E[j])
        E_all = np.array(E_all)

        # New density from the freshly solved states.
        new_q = q_from_orbitals([s[4] for s in states], [s[2] for s in states], r)
        # Mix old/new charge densities.
        q = (1.0 - mix) * q_prev + mix * new_q
        q_prev = q.copy()

        dE = 0.0 if E_prev is None else float(np.max(np.abs(E_all - E_prev)))
        dq = float(np.sqrt(np.mean((q - new_q) ** 2)))
        E_prev = E_all
        if verbose and (it % 5 == 0 or it == 1):
            labels = ["%d%s" % (s[0], 'spdf'[s[1]]) for s in states]
            print("  Z=%3d it=%3d  max|dE|=%.3e  rms(dq)=%.3e  E=%s"
                  % (z, it, dE, dq,
                     " ".join("%s:%.4f" % (lab, E) for lab, E in zip(labels, E_all))))

        if dE < e_tol and dq < q_tol and it > 1:
            break

    return {'z': z, 'config': config, 'r': r, 'dt': dt,
            'states': states, 'q': q, 'iterations': it}


def solve_element_relativistic(z, alpha=1.0, max_iter=80, mix=0.5,
                               grid=None, verbose=False):
    """Screened-potential solution with the RELATIVISTIC (radial Dirac)
    upgrade for the final states: run the nonrelativistic HFS SCF to
    convergence (see solve_element), then replace every subshell's radial
    function with the solution of the radial DIRAC equation in the
    converged potential (see pc/dirac_solver.py). One-shot (the potential
    is not re-self-consistened with the Dirac density -- a documented
    approximation; the density change under relativity is small for the
    radii this model reports, and the s/p contraction -- the physically
    dominant relativistic effect for Z >= 55 -- is captured).

    Per occupied (n, ell): l=0 has one Dirac state (kappa=-1, j=1/2);
    l>0 has two (j = l+1/2, kappa = -(l+1) and j = l-1/2, kappa = l),
    merged density-weighted by degeneracy (2j+1). The subshell's u(r) =
    sqrt(weighted P^2) with the sign of the dominant (j = l+1/2) component,
    and the eigenvalue is the weighted average.

    Returns the same dict shape as solve_element (states carry the Dirac
    u's and energies, plus 'relativistic': True).
    """
    res = solve_element(z, alpha=alpha, max_iter=max_iter, mix=mix,
                        grid=grid, verbose=verbose)
    r, dt = res['r'], res['dt']
    t = np.log(r / r[0])
    # converged potential from the converged density
    q = res['q']
    V = potential_from_q(z, r, q, alpha=alpha, latter=True)

    states = []
    for n, ell, occ, _E_nr, _u_nr in res['states']:
        if ell == 0:
            eps, P, Q = solve_dirac_state(V, r, t, dt, -1, n, z)
            u = np.array(P)
            E = eps
        else:
            k1 = -(ell + 1)  # j = l+1/2
            k2 = ell         # j = l-1/2
            eps1, P1, Q1 = solve_dirac_state(V, r, t, dt, k1, n, z)
            eps2, P2, Q2 = solve_dirac_state(V, r, t, dt, k2, n, z)
            w1 = (2 * ell + 2) / (4 * ell + 2)
            w2 = (2 * ell) / (4 * ell + 2)
            # merged radial density (weighted by degeneracy); sign from the
            # dominant j = l+1/2 component (P1)
            u2 = w1 * P1 * P1 + w2 * P2 * P2
            u = np.sqrt(np.maximum(u2, 0.0))
            u = np.where(P1 >= 0.0, u, -u)
            E = w1 * eps1 + w2 * eps2
        norm = np.trapezoid(u * u, r)
        if norm > 0.0:
            u = u / math.sqrt(norm)
            if u[0] < 0.0:
                u = -u
        states.append((n, ell, occ, float(E), u))
    res['states'] = states
    res['relativistic'] = True
    return res


def radial_mode_from_u(u, r):
    """Mode of r^2 R^2 = u^2 on the (log-spaced) grid, refined by parabolic
    interpolation around the argmax. Returns radius in Bohr."""
    w = u * u
    i = int(np.argmax(w))
    if 0 < i < len(w) - 1:
        # parabola through (r[i-1], w[i-1]), (r[i], w[i]), (r[i+1], w[i+1])
        x0, x1, x2 = r[i - 1], r[i], r[i + 1]
        y0, y1, y2 = w[i - 1], w[i], w[i + 1]
        denom = (x0 - x1) * (x0 - x2) * (x1 - x2)
        a = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / denom
        b = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / denom
        if abs(a) > 1e-30:
            r_peak = -b / (2.0 * a)
            if r[i - 1] < r_peak < r[i + 1]:
                return r_peak
    return r[i]


def valence_subshell(config):
    """Highest-l subshell among the highest-n occupied ones -- the subshell
    Clementi-Raimondi 'atomic radius' refers to (same as validate_atoms.py)."""
    n_max = max(n for n, _ell, _occ in config)
    return max(((n, ell) for n, ell, occ in config if n == n_max), key=lambda t: t[1])


def coulomb_check(grid=None):
    """Machinery gate: solve hydrogenic states in the EXACT -Z/r potential
    (empty density, no SCF) and compare E_n against the analytic
    E = -Z^2/(2n^2), and the mode of r^2 R^2 against the exact hydrogenic
    mode (computed by scanning the analytic radial function -- NOT n^2/Z,
    which is only the mode for circular states l = n-1; e.g. 2s mode is
    5.236 a0, 3p is 12 a0). Exercises the full grid + eigenproblem path
    with a known answer."""
    r, dt = grid if grid is not None else make_grid()
    z = 1
    V = -z / r
    ok = True
    print("coulomb check: hydrogenic states in exact -1/r potential")
    for ell in range(4):
        n_states = 3 if ell == 0 else 2
        E, U, interior = solve_states_l(V, r, dt, ell, n_states)
        for j in range(n_states):
            n = ell + 1 + j
            E_ana = -0.5 * z * z / (n * n)
            mode = radial_mode_from_u(U[:, j], r[interior])
            # exact hydrogenic mode: scan r^2 R(r)^2 on a fine linear grid
            coeff = orbitals.laguerre_coeffs(n, ell)
            rr = np.linspace(0.01, 4.0 * n * n, 200000)
            Rv = np.empty_like(rr)
            for k, ri in enumerate(rr):
                Rv[k] = orbitals.hydrogen_radial_function(ri, n, ell, coeff)
            w2 = (rr * Rv) ** 2
            mode_ana = rr[int(np.argmax(w2))]
            dE = abs(E[j] - E_ana) / abs(E_ana)
            dmode = abs(mode - mode_ana) / mode_ana
            flag = "OK" if (dE < 1e-4 and dmode < 1e-3) else "FAIL"
            if flag == "FAIL":
                ok = False
            print("  %d%s: E=%.9f (ana %.9f, rel err %.2e)  mode=%.6f a0 (ana %.6f, err %.2e)  %s"
                  % (n, 'spdf'[ell], E[j], E_ana, dE, mode, mode_ana, dmode, flag))
    return ok


def save_tables(results, out_path):
    """Write the converged solutions to an .npz, downsampled to a shared
    2001-point log grid (float32) -- plenty for the samplers and validation."""
    r_full = results[0]['r']
    r0, rmax = r_full[0], r_full[-1]
    n = 2001
    t = np.linspace(0.0, math.log(rmax / r0), n)
    r_out = r0 * np.exp(t)
    arrays = {'r': r_out.astype(np.float64)}
    z_list = []
    for res in results:
        z = res['z']
        z_list.append(z)
        arrays['z%d_config' % z] = np.array([(n, ell, occ) for n, ell, occ in res['config']],
                                            dtype=np.int32)
        for n, ell, occ, E, u in res['states']:
            # upsample u onto the output grid via plain linear interpolation
            # (u is smooth on a log grid).
            u_out = np.interp(r_out, res['r'], u)
            arrays['z%d_%d_%d_u' % (z, n, ell)] = u_out.astype(np.float32)
            arrays['z%d_%d_%d_E' % (z, n, ell)] = np.float64(E)
            arrays['z%d_%d_%d_occ' % (z, n, ell)] = np.int32(occ)
    arrays['z_list'] = np.array(z_list, dtype=np.int32)
    np.savez(out_path, **arrays)
    print("wrote %s (%d elements, %d subshells total)" % (
        out_path, len(z_list), sum(len(r['states']) for r in results)))


def load_results_from_npz(path):
    """Reconstruct solve_element() result dicts from a saved npz -- used by
    --resume to continue an interrupted batch without redoing elements."""
    out = []
    data = np.load(path)
    r = data['r']
    for z in data['z_list']:
        z = int(z)
        config = [tuple(int(v) for v in row) for row in data['z%d_config' % z]]
        states = []
        for n, ell, occ in config:
            u = data['z%d_%d_%d_u' % (z, n, ell)]
            E = float(data['z%d_%d_%d_E' % (z, n, ell)])
            states.append((n, ell, occ, E, u))
        out.append({'z': z, 'config': config, 'r': r, 'states': states,
                    'q': None, 'iterations': None})
    data.close()
    return out


def _solve_one_worker(args):
    """Module-level worker for ProcessPoolExecutor (must be picklable under
    Windows spawn). Builds its own grid; returns the solve_element result."""
    z, alpha, max_iter, mix, relativistic, rel_min = args
    if relativistic and z >= rel_min:
        res = solve_element_relativistic(z, alpha=alpha, max_iter=max_iter,
                                         mix=mix, grid=make_grid())
    else:
        res = solve_element(z, alpha=alpha, max_iter=max_iter, mix=mix,
                            grid=make_grid())
    return res


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--zmin', type=int, default=1)
    ap.add_argument('--zmax', type=int, default=118)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                  'hfs_tables.npz'))
    ap.add_argument('--alpha', type=float, default=1.0,
                    help='Slater exchange factor alpha (1.0 = Slater/HFS, 2/3 = Dirac/K-S)')
    ap.add_argument('--max-iter', type=int, default=80)
    ap.add_argument('--mix', type=float, default=0.35,
                    help='SCF density mixing fraction (0.35: stable for the '
                         'transition-metal 3d/4s; 0.5 drifts Fe into a '
                         'metastable diffuse-3d solution at alpha=2/3)')
    ap.add_argument('--relativistic', action='store_true',
                    help='solve the final states with the radial Dirac equation '
                         '(one-shot on the nonrelativistic SCF potential)')
    ap.add_argument('--rel-min', type=int, default=55,
                    help='with --relativistic: apply it only to Z >= this '
                         '(below, the Dirac solution is within ~1% of the '
                         'nonrelativistic one; default 55)')
    ap.add_argument('--jobs', type=int, default=1,
                    help='parallel elements (multiprocessing; 0 = all cores) '
                         '(sandbox note: blocked where named pipes are not '
                         'available)')
    ap.add_argument('--resume', action='store_true',
                    help='skip elements already present in --out (crash '
                         'recovery; pairs with the per-element incremental save)')
    ap.add_argument('--coulomb-check', action='store_true',
                    help='run the exact-hydrogen machinery gate and exit')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args(argv)

    if args.coulomb_check:
        ok = coulomb_check()
        return 0 if ok else 1

    grid = make_grid()
    z_range = list(range(args.zmin, args.zmax + 1))

    if args.jobs == 0 or args.jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        workers = os.cpu_count() if args.jobs == 0 else args.jobs
        tasks = [(z, args.alpha, args.max_iter, args.mix, args.relativistic,
                  args.rel_min) for z in z_range]
        with ProcessPoolExecutor(max_workers=workers) as ex:
            results = list(ex.map(_solve_one_worker, tasks))
        results.sort(key=lambda r: r['z'])
        save_tables(results, args.out)
        return 0

    # Serial path: per-element progress lines (flushed, so a live tail
    # always shows whether the tool is still working) and an incremental
    # save after every element (crash-safe; --resume continues from it).
    import time as _time
    all_results = []
    done_z = set()
    if args.resume and os.path.isfile(args.out):
        try:
            all_results = load_results_from_npz(args.out)
            done_z = set(r['z'] for r in all_results)
            print("resume: %d element(s) already in %s -- skipping them"
                  % (len(done_z), args.out), flush=True)
        except Exception as exc:  # noqa: BLE001 -- corrupt/partial file: start fresh
            print("resume: could not read %s (%s) -- starting fresh"
                  % (args.out, exc), flush=True)
            all_results = []
            done_z = set()
    pending = [z for z in z_range if z not in done_z]
    t_start = _time.time()
    for i, z in enumerate(pending):
        t0 = _time.time()
        if args.relativistic and z >= args.rel_min:
            res = solve_element_relativistic(z, alpha=args.alpha,
                                             max_iter=args.max_iter,
                                             mix=args.mix, grid=grid)
        else:
            res = solve_element(z, alpha=args.alpha, max_iter=args.max_iter,
                                mix=args.mix, grid=grid)
        all_results.append(res)
        n, ell = valence_subshell(res['config'])
        u = dict(((s[0], s[1]), s) for s in res['states'])[(n, ell)][4]
        mode = radial_mode_from_u(u, grid[0])
        dt_elem = _time.time() - t0
        frac = (len(done_z) + i + 1) / len(z_range)  # done before + this one
        elapsed = _time.time() - t_start
        eta = elapsed / max(frac, 1e-9) * (1.0 - frac)
        print("progress %4.1f%%  Z=%3d %-2s  %6.1fs (iters %d) | valence %d%s mode %6.1f pm | "
              "elapsed %6.0fs ETA %6.0fs"
              % (100.0 * frac, z, slater.element_symbol(z), dt_elem,
                 res['iterations'], n, 'spdf'[ell], mode * PM_PER_BOHR,
                 elapsed, eta), flush=True)
        # incremental save: the output npz always contains every completed
        # element, so an interruption loses at most the element in flight.
        all_results.sort(key=lambda r: r['z'])
        save_tables(all_results, args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
