"""Rejection-sampled point cloud generation for hydrogen orbitals, built on
top of the wavefunction engine in orbitals.py. This is the M2 step mentioned
in CLAUDE.md Sections 5/7: given a validated psi_real(), draw points
(x, y, z) distributed according to the probability density
|psi_{n,l,m}(r,theta,phi)|^2 * r^2 * sin(theta) (the r^2*sin(theta) factor is
the spherical-coordinates volume element, so points end up distributed in
*physical* probability, not just where |psi| happens to be large).

Samples r, theta, phi as three INDEPENDENT rejection samplers rather than one
joint 3D rejection sampler. This is exact, not an approximation: the target
density factors as
    |psi|^2 * r^2 * sin(theta) = [r*R(r)]^2 * [P_l^m(theta)^2*sin(theta)] * azimuthal(phi)^2
i.e. a product of three single-variable functions, so sampling each marginal
independently and combining the results reproduces the joint density
exactly. It's also much faster on an interpreted target like this one: a
joint 3D rejection sampler's acceptance rate is the *product* of what each
axis's rate would be alone (measured ~2% for a demanding orbital, i.e. ~50
wasted attempts per accepted point, each paying for a full R(r)*P(theta)
evaluation); sampling the axes separately needs only the sum of each axis's
much higher individual attempt count, and each attempt is cheaper too
(evaluates only R(r) or only P(theta), never both). See
tools/orbitals_host/README.md for the measured numbers that motivated this
(a prior joint-sampling version is preserved in git history).

Same three-way porting/cross-check discipline as orbitals.py: a small
portable PRNG (XorShift32) is mirrored bit-for-bit in src/pointcloud.h/.cpp
and tools/orbitals_host/js_reference.js, so the same seed produces the same
accepted point sequence across all three ports -- see tools/orbitals_host/
for the cross-check that relies on this.
"""

import math

import orbitals

# Multiplies each tabulated peak-magnitude bound by a small margin, since the
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
    """Precomputed data needed to repeatedly sample a given (n, ell, m)
    orbital without recomputing coefficients/tables/bounds on every draw.
    Build once with init_orbital_sampler(), then pass to
    sample_orbital_point() as many times as needed.
    """

    def __init__(self, n, ell, m, radial_coeff, legendre_coeff, max_r, max_r_weight, max_theta_weight):
        self.n = n
        self.ell = ell
        self.m = m
        self.radial_coeff = radial_coeff
        self.legendre_coeff = legendre_coeff
        self.max_r = max_r
        self.max_r_weight = max_r_weight        # Upper bound on [r*R(r)]^2.
        self.max_theta_weight = max_theta_weight  # Upper bound on P_l^m(theta)^2*sin(theta).


def init_orbital_sampler(n, ell, m):
    """Precompute an OrbitalSampler for (n, ell, m): coefficients (via
    orbitals.laguerre_coeffs()/legendre_coeffs()) plus valid upper bounds for
    each marginal's rejection envelope, obtained from the peak magnitudes of
    orbitals.build_radial_table()'s r*R(r) and
    orbitals.build_legendre_table()'s P_l^m(theta)^2*sin(theta) (each padded
    by DENSITY_BOUND_MARGIN).

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
    max_r_weight = 0.0
    for i, value in enumerate(radial_table):
        rr = value * (i * delta_r)
        w = rr * rr
        if w > max_r_weight:
            max_r_weight = w
    max_r_weight *= DENSITY_BOUND_MARGIN

    legendre_table = orbitals.build_legendre_table(ell, m)
    delta_theta = math.pi / (len(legendre_table) - 1)
    max_theta_weight = 0.0
    for i, value in enumerate(legendre_table):
        theta = i * delta_theta
        w = value * value * math.sin(theta)
        if w > max_theta_weight:
            max_theta_weight = w
    max_theta_weight *= DENSITY_BOUND_MARGIN

    return OrbitalSampler(n, ell, m, radial_coeff, legendre_coeff, max_r, max_r_weight, max_theta_weight)


def _sample_r(sampler, rng, max_attempts):
    """Rejection-sample r against the r-marginal density [r*R(r)]^2."""
    r = 0.0
    for _ in range(max_attempts):
        r = rng.uniform01() * sampler.max_r
        u = rng.uniform01() * sampler.max_r_weight
        radial = orbitals.hydrogen_radial_function(r, sampler.n, sampler.ell, sampler.radial_coeff)
        weight = (r * radial) * (r * radial)
        if u <= weight:
            break
    return r


def _sample_theta(sampler, rng, max_attempts):
    """Rejection-sample theta against P_l^m(theta)^2*sin(theta)."""
    theta = 0.0
    for _ in range(max_attempts):
        theta = rng.uniform01() * math.pi
        u = rng.uniform01() * sampler.max_theta_weight
        p_val = orbitals.compute_plm(theta, sampler.ell, sampler.m, sampler.legendre_coeff)
        weight = p_val * p_val * math.sin(theta)
        if u <= weight:
            break
    return theta


def _sample_phi(sampler, rng, max_attempts):
    """Sample phi: uniform directly when m==0 (azimuthal factor is constant
    1, no rejection needed), otherwise rejection-sample against
    cos^2(m*phi)/sin^2(m*phi) (envelope 1, so this needs ~2 attempts on
    average).
    """
    phi = rng.uniform01() * (2 * math.pi)
    if sampler.m == 0:
        return phi
    for _ in range(max_attempts):
        phi = rng.uniform01() * (2 * math.pi)
        u = rng.uniform01()
        if sampler.m >= 0:
            azimuthal = math.cos(sampler.m * phi)
        else:
            azimuthal = math.sin(-sampler.m * phi)
        if u <= azimuthal * azimuthal:
            break
    return phi


def sample_orbital_point(sampler, rng, max_attempts=100000):
    """Draw one point from the (r, theta, phi) probability density
    |psi_{n,l,m}|^2 * r^2 * sin(theta) by sampling r, theta and phi as three
    independent 1D rejection samplers (see the module docstring for why this
    is exact, not an approximation). Draw order per point is: resolve r (a
    rejection loop against [r*R(r)]^2), then resolve theta (a rejection loop
    against P_l^m(theta)^2*sin(theta)), then resolve phi (uniform directly if
    m==0; otherwise a rejection loop against cos^2(m*phi) or sin^2(m*phi)) --
    mirrored exactly in src/pointcloud.h/.cpp and
    tools/orbitals_host/js_reference.js so the same seed produces the same
    accepted point.

    Args:
        sampler: OrbitalSampler produced by init_orbital_sampler().
        rng: XorShift32 instance, advanced by this call.
        max_attempts: Safety cap on each axis's rejection-loop iterations;
            if exceeded for a given axis (which should not happen for
            correctly computed bounds), that axis falls back to its
            last-drawn candidate value.

    Returns:
        A tuple (x, y, z): an accepted point in Cartesian coordinates.
    """
    r = _sample_r(sampler, rng, max_attempts)
    theta = _sample_theta(sampler, rng, max_attempts)
    phi = _sample_phi(sampler, rng, max_attempts)

    sin_theta = math.sin(theta)
    x = r * sin_theta * math.cos(phi)
    y = r * sin_theta * math.sin(phi)
    z = r * math.cos(theta)
    return x, y, z
