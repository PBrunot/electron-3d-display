"""Rejection-sampled point cloud generation for hydrogen orbitals, built on
top of the wavefunction engine in orbitals.py. This is the M2 step mentioned
in CLAUDE.md Sections 5/7: given a validated psi_real(), draw points
(x, y, z) distributed according to the probability density
|psi_{n,l,m}(r,theta,phi)|^2 * r^2 * sin(theta) (the r^2*sin(theta) factor is
the spherical-coordinates volume element, so points end up distributed in
*physical* probability, not just where |psi| happens to be large).

Same three-way porting/cross-check discipline as orbitals.py: a small
portable PRNG (XorShift32) is mirrored bit-for-bit in src/pointcloud.h/.cpp
and tools/orbitals_host/js_reference.js, so the same seed produces the same
accepted point sequence across all three ports -- see tools/orbitals_host/
for the cross-check that relies on this.
"""

import math

import orbitals

# Multiplies the tabulated peak-magnitude bound by a small margin, since the
# true continuous peak of a smooth function can fall between two of the
# TABLE_SIZE tabulated points. Not a rigorous guarantee, but ample for a
# point-cloud visualization. Mirrored exactly (same literal) in
# src/pointcloud.cpp and tools/orbitals_host/js_reference.js.
DENSITY_BOUND_MARGIN = 1.15

_MASK32 = 0xFFFFFFFF


class XorShift32:
    """Portable xorshift32 PRNG (Marsaglia's (13,17,5) triple). Not
    cryptographically secure -- chosen only because it is trivial to port
    bit-for-bit to C++ and JavaScript, which is what lets the point clouds
    produced by all three ports be compared for exact agreement rather than
    just statistically. Python/MicroPython ints are arbitrary-precision, so
    each step is masked to 32 bits explicitly (unlike C++'s uint32_t, which
    wraps automatically).
    """

    def __init__(self, seed):
        self.state = seed if seed != 0 else 1

    def next(self):
        """Return the next raw 32-bit value in the sequence, advancing state."""
        x = self.state
        x = (x ^ (x << 13)) & _MASK32
        x = (x ^ (x >> 17)) & _MASK32
        x = (x ^ (x << 5)) & _MASK32
        self.state = x
        return x

    def uniform01(self):
        """Return the next value mapped to [0, 1)."""
        return self.next() / 4294967296.0  # 2**32


class OrbitalSampler:
    """Precomputed data needed to repeatedly rejection-sample a given
    (n, ell, m) orbital without recomputing coefficients/tables on every
    draw. Build once with init_orbital_sampler(), then pass to
    sample_orbital_point() as many times as needed.
    """

    def __init__(self, n, ell, m, radial_coeff, legendre_coeff, max_r, density_bound):
        self.n = n
        self.ell = ell
        self.m = m
        self.radial_coeff = radial_coeff
        self.legendre_coeff = legendre_coeff
        self.max_r = max_r
        self.density_bound = density_bound


def init_orbital_sampler(n, ell, m):
    """Precompute an OrbitalSampler for (n, ell, m): coefficients (via
    orbitals.laguerre_coeffs()/legendre_coeffs()) plus a valid upper bound
    on the target density, obtained from the peak magnitudes of
    orbitals.build_radial_table()'s r*R(r) and
    orbitals.build_legendre_table()'s P_l^m(theta) (each padded by
    DENSITY_BOUND_MARGIN).

    Args:
        n: Principal quantum number.
        ell: Angular momentum quantum number, 0 <= ell <= n-1.
        m: Magnetic quantum number, -ell <= m <= ell.

    Returns:
        An OrbitalSampler ready for sample_orbital_point().
    """
    radial_coeff = orbitals.laguerre_coeffs(n, ell)
    legendre_coeff = orbitals.legendre_coeffs(ell, m)

    radial_table, max_r = orbitals.build_radial_table(n, ell)
    delta_r = max_r / (len(radial_table) - 1)
    max_rr = 0.0
    for i, value in enumerate(radial_table):
        rr = abs(value * (i * delta_r))
        if rr > max_rr:
            max_rr = rr

    legendre_table = orbitals.build_legendre_table(ell, m)
    max_p = 0.0
    for value in legendre_table:
        p = abs(value)
        if p > max_p:
            max_p = p

    density_bound = max_rr * max_rr * max_p * max_p * DENSITY_BOUND_MARGIN
    return OrbitalSampler(n, ell, m, radial_coeff, legendre_coeff, max_r, density_bound)


def sample_orbital_point(sampler, rng, max_attempts=100000):
    """Draw one point from the (r, theta, phi) probability density
    |psi_{n,l,m}|^2 * r^2 * sin(theta) via rejection sampling: repeatedly
    draw a uniform random (r, theta, phi) candidate and a uniform random
    threshold in [0, sampler.density_bound], accepting the candidate once
    the threshold falls under its actual density. Number of RNG draws per
    accepted point is variable (that's the nature of rejection sampling)
    but bounded by max_attempts.

    Args:
        sampler: OrbitalSampler produced by init_orbital_sampler().
        rng: XorShift32 instance, advanced by this call.
        max_attempts: Safety cap on rejection-loop iterations; if exceeded
            (which should not happen for a correctly computed
            density_bound), returns the origin (0, 0, 0).

    Returns:
        A tuple (x, y, z): an accepted point in Cartesian coordinates.
    """
    for _ in range(max_attempts):
        # Fixed draw order (r, theta, phi, u) -- mirrored exactly in the C++
        # and JS ports so identical seeds produce identical accept/reject
        # sequences and identical accepted points.
        r = rng.uniform01() * sampler.max_r
        theta = rng.uniform01() * math.pi
        phi = rng.uniform01() * (2 * math.pi)
        u = rng.uniform01() * sampler.density_bound

        psi = orbitals.psi_real(r, theta, phi, sampler.n, sampler.ell, sampler.m, sampler.radial_coeff,
                                 sampler.legendre_coeff)
        density = (psi * r) * (psi * r) * math.sin(theta)

        if u <= density:
            sin_theta = math.sin(theta)
            x = r * sin_theta * math.cos(phi)
            y = r * sin_theta * math.sin(phi)
            z = r * math.cos(theta)
            return x, y, z
    return 0.0, 0.0, 0.0
