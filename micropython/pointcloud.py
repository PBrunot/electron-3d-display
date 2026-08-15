"""Point cloud generation for hydrogen orbitals, built on top of the
wavefunction engine in orbitals.py. This is the M2 step mentioned in
CLAUDE.md Sections 5/7: given a validated psi_real(), draw points
(x, y, z) distributed according to the probability density
|psi_{n,l,m}(r,theta,phi)|^2 * r^2 * sin(theta) (the r^2*sin(theta) factor is
the spherical-coordinates volume element, so points end up distributed in
*physical* probability, not just where |psi| happens to be large).

Samples r, theta, phi from three INDEPENDENT precomputed inverse-CDF
(quantile) tables rather than rejection sampling. This is exact, not an
approximation: the target density factors as
    |psi|^2 * r^2 * sin(theta) = [r*R(r)]^2 * [P_l^m(theta)^2*sin(theta)] * azimuthal(phi)^2
i.e. a product of three single-variable functions, so sampling each marginal
independently and combining the results reproduces the joint density exactly
(the same factorization a prior separable-rejection version -- preserved in
git history -- exploited). Each marginal's inverse CDF is built once per
orbital with a single monotonic forward sweep (no per-point search), so
sampling a point costs exactly three table lookups (interpolated, O(1)) plus
the trig to convert to Cartesian -- no rejection loop anywhere, so no
variance in per-point cost and no interpreter loop overhead per point. This
refines an earlier per-axis *rejection* version (also preserved in git
history) the same way stef1949/Electron-Orbital-Simulator's GPU sampler does
it: precompute the inverse function itself, not just the CDF, so there's no
search at sample time either.

Same three-way porting/cross-check discipline as orbitals.py: a small
portable PRNG (XorShift32) is mirrored bit-for-bit in src/pointcloud.h/.cpp
and tools/orbitals_host/js_reference.js, so the same seed produces the same
accepted point sequence across all three ports -- see tools/orbitals_host/
for the cross-check that relies on this.
"""

import math

import orbitals

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
    orbital without recomputing anything on every draw. Build once with
    init_orbital_sampler(), then pass to sample_orbital_point() as many
    times as needed. n/ell/m are kept only as caller-convenience metadata
    (e.g. for a UI label) -- sampling itself never needs them again once the
    tables below are built.
    """

    def __init__(self, n, ell, m, max_r, inv_r_table, inv_theta_table, inv_phi_table):
        self.n = n
        self.ell = ell
        self.m = m
        self.max_r = max_r
        self.inv_r_table = inv_r_table        # Inverse CDF of [r*R(r)]^2.
        self.inv_theta_table = inv_theta_table  # Inverse CDF of P_l^m(theta)^2*sin(theta).
        self.inv_phi_table = inv_phi_table      # Inverse CDF of azimuthal(phi)^2.


def _build_inverse_cdf(weight, domain_max):
    """Given non-negative sample weights of a density over [0, domain_max]
    taken at evenly spaced points, build the inverse CDF: result[k] is the
    x-value at quantile k/(len(weight)-1). Both the forward cumulative sum
    and the inverse lookup below are single monotonic sweeps (the CDF is
    non-decreasing, and the target quantile is non-decreasing in k), so this
    is O(len(weight)) total -- no per-point search survives into sampling
    either, since the result is later read directly via
    orbitals.get_value_from_lookup_table().
    """
    count = len(weight)
    delta = domain_max / (count - 1)

    cdf = [0.0] * count
    cumulative = 0.0
    for i in range(count):
        cumulative += weight[i]
        cdf[i] = cumulative
    total = cdf[-1]
    if total <= 0.0:
        total = 1.0  # degenerate guard; shouldn't occur for valid quantum numbers
    for i in range(count):
        cdf[i] /= total

    inv_table = [0.0] * count
    j = 0
    for k in range(count):
        u = k / (count - 1)
        while j < count - 1 and cdf[j] < u:
            j += 1
        j0 = j - 1 if j > 0 else 0
        j1 = j
        c0 = cdf[j0]
        c1 = cdf[j1]
        t = (u - c0) / (c1 - c0) if c1 > c0 else 0.0
        inv_table[k] = (j0 + t * (j1 - j0)) * delta
    return inv_table


def init_orbital_sampler(n, ell, m):
    """Precompute an OrbitalSampler for (n, ell, m): builds the three
    inverse-CDF tables above from orbitals.hydrogen_radial_function()'s
    r*R(r), orbitals.compute_plm()'s P_l^m(theta), and the azimuthal factor
    cos(m*phi)/sin(|m|*phi) (evaluated directly, no separate table
    dependency).

    Args:
        n: Principal quantum number.
        ell: Angular momentum quantum number, 0 <= ell <= n-1.
        m: Magnetic quantum number, -ell <= m <= ell.

    Returns:
        An OrbitalSampler ready for sample_orbital_point().
    """
    radial_coeff = orbitals.laguerre_coeffs(n, ell)
    legendre_coeff = orbitals.legendre_coeffs(ell, m)

    max_r = 6 * n * n
    table_size = orbitals.TABLE_SIZE

    delta_r = max_r / (table_size - 1)
    r_weight = [0.0] * table_size
    for i in range(table_size):
        r = i * delta_r
        radial = orbitals.hydrogen_radial_function(r, n, ell, radial_coeff)
        r_weight[i] = (r * radial) * (r * radial)
    inv_r_table = _build_inverse_cdf(r_weight, max_r)

    delta_theta = math.pi / (table_size - 1)
    theta_weight = [0.0] * table_size
    for i in range(table_size):
        theta = i * delta_theta
        p_val = orbitals.compute_plm(theta, ell, m, legendre_coeff)
        theta_weight[i] = p_val * p_val * math.sin(theta)
    inv_theta_table = _build_inverse_cdf(theta_weight, math.pi)

    two_pi = 2 * math.pi
    delta_phi = two_pi / (table_size - 1)
    phi_weight = [0.0] * table_size
    for i in range(table_size):
        phi = i * delta_phi
        if m >= 0:
            azimuthal = math.cos(m * phi)
        else:
            azimuthal = math.sin(-m * phi)
        phi_weight[i] = azimuthal * azimuthal  # m==0 -> constant 1, uniform phi
    inv_phi_table = _build_inverse_cdf(phi_weight, two_pi)

    return OrbitalSampler(n, ell, m, max_r, inv_r_table, inv_theta_table, inv_phi_table)


def sample_orbital_point(sampler, rng):
    """Draw one point from the (r, theta, phi) probability density
    |psi_{n,l,m}|^2 * r^2 * sin(theta) via inverse-CDF (quantile) sampling:
    one uniform draw per axis, mapped through that axis's precomputed
    inverse table via linear interpolation
    (orbitals.get_value_from_lookup_table()). Always exactly 3 RNG draws per
    point, in the fixed order (r, theta, phi) -- mirrored exactly in
    src/pointcloud.h/.cpp and tools/orbitals_host/js_reference.js so the
    same seed produces the same point.

    Args:
        sampler: OrbitalSampler produced by init_orbital_sampler().
        rng: XorShift32 instance, advanced by this call.

    Returns:
        A tuple (x, y, z): the sampled point in Cartesian coordinates.
    """
    r = orbitals.get_value_from_lookup_table(rng.uniform01(), sampler.inv_r_table)
    theta = orbitals.get_value_from_lookup_table(rng.uniform01(), sampler.inv_theta_table)
    phi = orbitals.get_value_from_lookup_table(rng.uniform01(), sampler.inv_phi_table)

    sin_theta = math.sin(theta)
    x = r * sin_theta * math.cos(phi)
    y = r * sin_theta * math.sin(phi)
    z = r * math.cos(theta)
    return x, y, z
