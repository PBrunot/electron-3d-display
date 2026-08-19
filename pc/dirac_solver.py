#!/usr/bin/env python3
"""Radial Dirac solver for the screened-potential (HFS) atom model -- the
relativistic upgrade (R3) of pc/hfs_solver.py for Z >= ~55, where s/p
orbitals contract (kinetic energies comparable to mc^2). See
pc/screened_potential_model.md section 2.

Solves, for each occupied (n, kappa) state in a given central potential
V(r) (Hartree atomic units, c = 1/alpha = 137.035999084):

    dP/dr = -(kappa/r) P + (2c + (eps - V)/c) Q
    dQ/dr =  (kappa/r) Q - ((eps - V)/c) P

with P = r*g (large component), Q = r*f (small), eps the binding energy
(negative, relative to rest mass), kappa = -(l+1) for j = l+1/2 and
kappa = +l for j = l-1/2. Near the origin P, Q ~ r^gamma with
gamma = sqrt(kappa^2 - (Z alpha)^2); at infinity both decay.

Eigenvalue: outward shooting on the log-uniform grid (r = r0 e^t), RK4 in
t, with the Sturm count (number of nodes of P) as the bisection target: the
j-th eigenvalue from the bottom (j = n - |kappa|) is the point where the
node count jumps. This needs no tail matching and is robust.

Validation gate (exact result): for V = -Z/r the Dirac eigenvalues are known
analytically,  eps_{n,kappa} = m c^2 [ 1 + (Z alpha)^2 / (n - |kappa| +
sqrt(kappa^2 - (Z alpha)^2))^2 ]^(-1/2) - m c^2
which for Z=1, 1s gives m c^2 (sqrt(1-alpha^2) - 1) ~ -0.49993 Ha.
"""

import math

import numpy as np

ALPHA = 1.0 / 137.035999084
ALPHA_INV = 137.035999084
C = ALPHA_INV  # speed of light in Hartree atomic units


def kappa_for(l, j_plus):  # j_plus: True -> j = l+1/2 (kappa = -(l+1))
    return -(l + 1) if j_plus else l


def _ic(V0, kappa, z, r0):
    """Power-law initial conditions at r0: P = r0^gamma, Q = P * c(gamma+kappa)/Z."""
    gamma = math.sqrt(kappa * kappa - (z * ALPHA) ** 2)
    p0 = r0 ** gamma
    q0 = p0 * C * (gamma + kappa) / z
    return p0, q0


def shoot(V, r, t, dt, kappa, eps, z):
    """Outward RK4 integration of the radial Dirac equations in t, for trial
    energy eps. Returns (P, Q, n_valid): P, Q on the grid (P > 0 near the
    origin) and n_valid = number of grid points integrated before the
    solution diverged (|P| > 1e100) -- for deep states the growing branch
    always dominates far out in the forbidden region, so the eigenfunction
    is only meaningful on [0, n_valid); the density there is the physical
    one (the true wavefunction is negligible beyond)."""
    n = len(r)
    P = np.empty(n)
    Q = np.empty(n)
    p, q = _ic(V[0], kappa, z, r[0])
    P[0] = p
    Q[0] = q
    n_valid = n
    for i in range(n - 1):
        ri = r[i]
        # dP/dt = -kappa P + r (2c + (eps-V)/c) Q
        # dQ/dt =  kappa Q - r ((eps-V)/c) P
        def f(p_, q_, rr, vv):
            w = (eps - vv) / C
            return (-kappa * p_ + rr * (2.0 * C + w) * q_,
                    kappa * q_ - rr * w * p_)
        k1p, k1q = f(p, q, ri, V[i])
        k2p, k2q = f(p + 0.5 * dt * k1p, q + 0.5 * dt * k1q, ri, V[i])
        k3p, k3q = f(p + 0.5 * dt * k2p, q + 0.5 * dt * k2q, ri, V[i])
        k4p, k4q = f(p + dt * k3p, q + dt * k3q, r[i + 1], V[i + 1])
        p += dt * (k1p + 2 * k2p + 2 * k3p + k4p) / 6.0
        q += dt * (k1q + 2 * k2q + 2 * k3q + k4q) / 6.0
        P[i + 1] = p
        Q[i + 1] = q
        if abs(p) > 1e100 or not math.isfinite(p):
            n_valid = i + 1
            # Zero the unintegrated tail -- P/Q beyond n_valid are
            # uninitialized np.empty garbage that node_count() would
            # otherwise read as spurious sign changes (nondeterministic!).
            P[n_valid:] = 0.0
            Q[n_valid:] = 0.0
            break
    return P, Q, n_valid


def node_count(P):
    """Number of sign changes of P on the grid."""
    cnt = 0
    for i in range(1, len(P)):
        if P[i - 1] * P[i] < 0.0:
            cnt += 1
    return cnt


def solve_dirac_state(V, r, t, dt, kappa, n, z, eps_lo=None, eps_hi=None,
                      max_iter=80, tol=1e-10):
    """Eigenvalue + eigenfunction of radial Dirac state (n, kappa) in V, by
    bisection on the Sturm count (nodes of P). Returns (eps, P, Q) with P
    normalized to int (P^2 + Q^2) dr = 1.

    The j-th state from the bottom (j = n - |kappa| + (1 if kappa < 0 else
    0), because for kappa < 0 the principal quantum number starts at |kappa|
    -- 1s1/2 is the first s state -- while for kappa > 0 it starts at
    |kappa|+1 -- 2p1/2 is the first p1/2 state) is found as the point where
    node_count(eps) jumps from j-1 to j: bisect on node_count < j.
    """
    j = n - abs(kappa) + (1 if kappa < 0 else 0)
    lo = eps_lo if eps_lo is not None else -2.0 * z * z - 10.0
    hi = eps_hi if eps_hi is not None else -1e-8

    def count(eps):
        P, Q, _nv = shoot(V, r, t, dt, kappa, eps, z)
        return node_count(P)

    # Sanity: lo must have count < j and hi must have count >= j
    c_lo = count(lo)
    c_hi = count(hi)
    if c_lo >= j:
        raise RuntimeError("dirac: lower bracket not deep enough (z=%d kappa=%d n=%d: count(lo)=%d >= %d)"
                           % (z, kappa, n, c_lo, j))
    if c_hi < j:
        raise RuntimeError("dirac: no bound state n=%d kappa=%d (count(hi)=%d < %d)"
                           % (n, kappa, c_hi, j))

    for _ in range(max_iter):
        mid = 0.5 * (lo + hi)
        if count(mid) >= j:
            hi = mid
        else:
            lo = mid
        if hi - lo < tol:
            break
    eps = 0.5 * (lo + hi)
    P, Q = _matched_wavefunction(V, r, t, dt, kappa, eps, z)
    return eps, P, Q


def _matched_wavefunction(V, r, t, dt, kappa, eps, z, margin_decay_lengths=6.0):
    """Two-sided eigenfunction for eigenvalue eps: outward from the origin
    (power law) to a matching point in the classically forbidden region,
    inward from rmax (decaying asymptotic form, stable under backward
    integration), scaled to match P at the matching point.

    The outward-only solution is unusable at large r: the tiny growing-branch
    contamination from the finite eigenvalue accuracy (delta ~ 1e-10)
    dominates there (it grows like exp(+lambda r)), so the naive P^2 mode
    lands in the numerical tail (seen: Au 6s mode at 22 a0 instead of
    ~0.75). Matching removes it.

    Returns P, Q on the full grid, normalized to int (P^2 + Q^2) dr = 1.
    """
    n = len(r)
    # 1) outward solution (may diverge far out; only used up to r_m)
    P_out, Q_out, n_valid = shoot(V, r, t, dt, kappa, eps, z)
    lam = math.sqrt(max(-2.0 * eps, 1e-12))
    # outer classical turning point: the largest r where V < eps (classically
    # allowed inside, forbidden outside -- V is monotone rising to ~0).
    i_tp = 0
    for i in range(n):
        if V[i] < eps:
            i_tp = i
    r_tp = r[i_tp]
    r_m = min(r_tp + margin_decay_lengths / lam, r[n_valid - 1] if n_valid < n else r[-1])
    # find the grid index of r_m
    i_m = int(np.searchsorted(r, r_m))
    i_m = max(1, min(n - 2, i_m))
    r_m = r[i_m]

    # 2) inward solution from a start radius a few decay lengths beyond r_m,
    #    with the decaying asymptotic form (Coulomb tail V ~ -Z_tail/r:
    #    P ~ r^nu e^{-lam r}, nu = Z_tail/lam). Start at min(rmax,
    #    r_m + 20/lam) so P(start) stays representable (no underflow), then
    #    integrate backward to r_m (stable on the decaying branch).
    z_tail = max(-V[-1] * r[-1], 1.0)  # effective tail charge (Latter: 1)
    nu = z_tail / lam
    r_start = min(r[-1], r_m + 20.0 / lam)
    i_start = int(np.searchsorted(r, r_start))
    i_start = max(i_m + 1, min(n - 1, i_start))
    r_start = r[i_start]
    p = r_start ** nu * math.exp(-lam * r_start)
    pp = p * (nu / r_start - lam)
    q = (pp + kappa * p / r_start) / (2.0 * C + (eps - V[i_start]) / C)
    P_in = np.zeros(n)
    Q_in = np.zeros(n)
    P_in[i_start] = p
    Q_in[i_start] = q
    for i in range(i_start, i_m, -1):
        ri = r[i]

        def f(p_, q_, rr, vv):
            w = (eps - vv) / C
            return (-kappa * p_ + rr * (2.0 * C + w) * q_,
                    kappa * q_ - rr * w * p_)

        h = -dt  # integrate backward in t
        k1p, k1q = f(p, q, ri, V[i])
        k2p, k2q = f(p + 0.5 * h * k1p, q + 0.5 * h * k1q, ri, V[i])
        k3p, k3q = f(p + 0.5 * h * k2p, q + 0.5 * h * k2q, ri, V[i])
        k4p, k4q = f(p + h * k3p, q + h * k3q, r[i - 1], V[i - 1])
        p += h * (k1p + 2 * k2p + 2 * k3p + k4p) / 6.0
        q += h * (k1q + 2 * k2q + 2 * k3q + k4q) / 6.0
        P_in[i - 1] = p
        Q_in[i - 1] = q
        if abs(p) > 1e100 or not math.isfinite(p):
            # backward integration diverged (should not happen on the
            # decaying branch); degrade to the outward-only solution up to
            # the divergence point, normalized.
            P = np.zeros(n)
            Q = np.zeros(n)
            P[:n_valid] = P_out[:n_valid]
            Q[:n_valid] = Q_out[:n_valid]
            norm = np.trapezoid(P * P + Q * Q, r)
            if norm > 0.0:
                P = P / math.sqrt(norm)
                Q = Q / math.sqrt(norm)
            return P, Q

    # 3) scale the inward piece to match P at r_m, splice
    scale = P_out[i_m] / P_in[i_m] if abs(P_in[i_m]) > 1e-300 else 0.0
    P = np.empty(n)
    Q = np.empty(n)
    P[:i_m + 1] = P_out[:i_m + 1]
    Q[:i_m + 1] = Q_out[:i_m + 1]
    P[i_m + 1:] = scale * P_in[i_m + 1:]
    Q[i_m + 1:] = scale * Q_in[i_m + 1:]
    # 4) normalize over the full grid
    norm = np.trapezoid(P * P + Q * Q, r)
    if norm > 0.0:
        P = P / math.sqrt(norm)
        Q = Q / math.sqrt(norm)
    return P, Q


def dirac_energy_analytic(z, n, kappa):
    """Exact Dirac hydrogenic binding energy (Hartree), for validation."""
    za = z * ALPHA
    rad = n - abs(kappa) + math.sqrt(kappa * kappa - za * za)
    return C * C * ((1.0 + (za / rad) ** 2) ** -0.5 - 1.0)


def dirac_validation(z=1, n_max=3):
    """Gate: hydrogenic Dirac energies vs the analytic formula."""
    r, t = None, None
    import sys
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from hfs_solver import make_grid
    r, dt = make_grid()
    t = np.log(r / r[0])
    V = -z / r
    ok = True
    print("dirac check: hydrogenic eigenvalues vs analytic (Z=%d)" % z)
    for kappa in (-1, 1, -2):
        for n in range(abs(kappa) + 1, min(n_max, abs(kappa) + 3) + 1):
            eps, P, Q = solve_dirac_state(V, r, t, dt, kappa, n, z)
            ana = dirac_energy_analytic(z, n, kappa)
            err = abs(eps - ana) / abs(ana)
            flag = "OK" if err < 1e-6 else "FAIL"
            if flag == "FAIL":
                ok = False
            print("  n=%d kappa=%+d: eps=%.10f (ana %.10f, rel err %.2e)  %s"
                  % (n, kappa, eps, ana, err, flag))
    return ok


if __name__ == '__main__':
    import sys
    sys.exit(0 if dirac_validation() else 1)
