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

Optimized per docs.micropython.org/en/latest/reference/speed_python.html,
same techniques as orbitals.py (see that module's docstring for the
`@micropython.native` / CPython-compatibility tradeoff, which applies here
too), plus one more: the three inverse-CDF tables and the scratch weight/cdf
arrays used to build them are `array.array('d', ...)` instead of `list`.
MicroPython's `list` stores each element as a separately heap-allocated
boxed object; `array.array` packs raw doubles into one contiguous buffer, so
building a table (1001 elements, done 3x per orbital in init_orbital_sampler
plus 2x more per table in _build_inverse_cdf's scratch cdf/inv_table -- ~9000
element-writes total per orbital) avoids thousands of small allocations and
the GC pressure they create. Typecode 'd' (not 'f') is deliberate: on the
unix port (double-precision floats, used for the tight cross-check gate)
'f' would silently truncate every stored value to float32, masking real
precision loss as if it were a formatting artifact; 'd' preserves full
double precision there. On the actual ESP32 build (float32-only), 'd' costs
nothing extra -- every float on that build is already single-precision
under the hood, array typecode notwithstanding (verified empirically: 'd'
and 'f' arrays round-trip identically on that board).

Viper (the other code-emitter docs.micropython.org describes, promising
still more speed for integer/bitwise code -- and XorShift32.next() is
exactly that) was deliberately NOT applied here. Its typed integers are a
different underlying representation than plain Python ints, and this
project's entire cross-language validation strategy depends on XorShift32
producing bit-identical output to the C++ and JS ports for the same seed;
a subtle wraparound/representation mismatch in viper's integer semantics
could silently break that without changing any *visible* behavior enough to
notice casually. Not worth the risk for one function already sped up by
`@micropython.native` (which does not change integer semantics, only how
the same operations are executed) -- revisit only if profiling on real
hardware shows XorShift32 itself is now the bottleneck.
"""

import array
import math

import orbitals

_MASK32 = 0xFFFFFFFF

_sin = math.sin
_cos = math.cos
_sqrt = math.sqrt
_pi = math.pi

# Stable function references, cached at module scope to avoid an
# "orbitals" global lookup plus an attribute lookup on every call inside
# the ~1001-iteration loops in init_orbital_sampler() and on every call to
# sample_orbital_point() (which runs once per generated point).
_hydrogen_radial_function = orbitals.hydrogen_radial_function
_compute_plm = orbitals.compute_plm
_get_value_from_lookup_table = orbitals.get_value_from_lookup_table


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

    @micropython.native
    def next(self):
        """Return the next raw 32-bit value in the sequence, advancing state."""
        x = self.state
        x = (x ^ (x << 13)) & _MASK32
        x = (x ^ (x >> 17)) & _MASK32
        x = (x ^ (x << 5)) & _MASK32
        self.state = x
        return x

    @micropython.native
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


@micropython.native
def _build_inverse_cdf(weight, domain_max):
    """Given non-negative sample weights of a density over [0, domain_max]
    taken at evenly spaced points, build the inverse CDF: result[k] is the
    x-value at quantile k/(len(weight)-1). Both the forward cumulative sum
    and the inverse lookup below are single monotonic sweeps (the CDF is
    non-decreasing, and the target quantile is non-decreasing in k), so this
    is O(len(weight)) total -- no per-point search survives into sampling
    either, since the result is later read directly via
    get_value_from_lookup_table().
    """
    count = len(weight)
    delta = domain_max / (count - 1)

    cdf = array.array('d', bytes(8 * count))
    cumulative = 0.0
    for i in range(count):
        cumulative += weight[i]
        cdf[i] = cumulative
    total = cdf[count - 1]
    if total <= 0.0:
        total = 1.0  # degenerate guard; shouldn't occur for valid quantum numbers
    for i in range(count):
        cdf[i] /= total

    inv_table = array.array('d', bytes(8 * count))
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


@micropython.native
def init_orbital_sampler(n, ell, m, z_eff=1.0):
    """Precompute an OrbitalSampler for (n, ell, m): builds the three
    inverse-CDF tables above from orbitals.hydrogen_radial_function()'s
    r*R(r), orbitals.compute_plm()'s P_l^m(theta), and the azimuthal factor
    cos(m*phi)/sin(|m|*phi) (evaluated directly, no separate table
    dependency).

    Args:
        n: Principal quantum number.
        ell: Angular momentum quantum number, 0 <= ell <= n-1.
        m: Magnetic quantum number, -ell <= m <= ell.
        z_eff: Effective nuclear charge (see slater.py), default 1.0 (plain
            hydrogen, unchanged behavior). Only the RADIAL table is
            affected -- same r -> z_eff*r substitution and max_r=6n^2/z_eff
            shrink as init_radial_sampler() below; the angular tables never
            depend on Z at all (a hydrogenic wavefunction's angular part
            doesn't change with nuclear charge, only its radial extent
            does), so theta/phi are untouched.

    Returns:
        An OrbitalSampler ready for sample_orbital_point().
    """
    radial_coeff = orbitals.laguerre_coeffs(n, ell)
    legendre_coeff = orbitals.legendre_coeffs(ell, m)
    radial_fn = _hydrogen_radial_function  # local alias: fastest possible lookup inside the loops below
    plm_fn = _compute_plm
    sin = _sin
    cos = _cos

    max_r = 6 * n * n / z_eff
    table_size = orbitals.TABLE_SIZE

    delta_r = max_r / (table_size - 1)
    r_weight = array.array('d', bytes(8 * table_size))
    for i in range(table_size):
        r = i * delta_r
        radial = radial_fn(z_eff * r, n, ell, radial_coeff)
        r_weight[i] = (r * radial) * (r * radial)
    inv_r_table = _build_inverse_cdf(r_weight, max_r)

    delta_theta = _pi / (table_size - 1)
    theta_weight = array.array('d', bytes(8 * table_size))
    for i in range(table_size):
        theta = i * delta_theta
        p_val = plm_fn(theta, ell, m, legendre_coeff)
        theta_weight[i] = p_val * p_val * sin(theta)
    inv_theta_table = _build_inverse_cdf(theta_weight, _pi)

    two_pi = 2 * _pi
    delta_phi = two_pi / (table_size - 1)
    phi_weight = array.array('d', bytes(8 * table_size))
    for i in range(table_size):
        phi = i * delta_phi
        if m >= 0:
            azimuthal = cos(m * phi)
        else:
            azimuthal = sin(-m * phi)
        phi_weight[i] = azimuthal * azimuthal  # m==0 -> constant 1, uniform phi
    inv_phi_table = _build_inverse_cdf(phi_weight, two_pi)

    return OrbitalSampler(n, ell, m, max_r, inv_r_table, inv_theta_table, inv_phi_table)


@micropython.native
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
    lookup = _get_value_from_lookup_table
    r = lookup(rng.uniform01(), sampler.inv_r_table)
    theta = lookup(rng.uniform01(), sampler.inv_theta_table)
    phi = lookup(rng.uniform01(), sampler.inv_phi_table)

    sin_theta = _sin(theta)
    x = r * sin_theta * _cos(phi)
    y = r * sin_theta * _sin(phi)
    z = r * _cos(theta)
    return x, y, z


@micropython.native
def init_radial_sampler(n, ell, z_eff=1.0):
    """Build just the radial inverse-CDF table used by
    sample_isotropic_point() below, for a hydrogenic subshell (n, ell)
    scaled by effective nuclear charge z_eff (see slater.py) instead of the
    real bound electron this module otherwise assumes. The Z-dependence of a
    hydrogenic radial function is exactly the variable substitution
    r -> z_eff*r (see slater.py's module docstring) -- orbitals.py itself
    needs no change for this, only the r fed into it here and the covered
    range (denser/closer-in wavefunctions need a smaller max_r to keep the
    same table resolution).

    No angular table here -- unlike init_orbital_sampler(), this never
    touches legendre_coeffs()/m at all: atom_cloud.py samples direction
    uniformly over the sphere instead (see sample_isotropic_point()).

    Returns (inv_r_table, max_r): max_r is kept only as caller-facing
    metadata (e.g. for a UI/debug readout) -- sample_isotropic_point() only
    needs inv_r_table, same as sample_orbital_point() never reads
    sampler.max_r either.
    """
    coeff = orbitals.laguerre_coeffs(n, ell)
    radial_fn = _hydrogen_radial_function

    max_r = 6 * n * n / z_eff
    table_size = orbitals.TABLE_SIZE
    delta_r = max_r / (table_size - 1)
    r_weight = array.array('d', bytes(8 * table_size))
    for i in range(table_size):
        r = i * delta_r
        radial = radial_fn(z_eff * r, n, ell, coeff)
        r_weight[i] = (r * radial) * (r * radial)
    inv_r_table = _build_inverse_cdf(r_weight, max_r)
    return inv_r_table, max_r


@micropython.native
def sample_isotropic_point(inv_r_table, rng):
    """Draw one point from a spherically-symmetric density whose radial part
    is inv_r_table (from init_radial_sampler()) and whose angular part is
    uniform over the sphere -- the spherically-averaged-subshell
    approximation atom_cloud.py uses for multi-electron atoms. Exact for a
    FULL subshell (Unsoeld's theorem: summing |Y_l^m|^2 over every m in a
    full subshell gives a constant); used here as an approximation for
    partially-filled subshells too -- see atom_cloud.py's module docstring.

    Same 3-draws-per-point discipline as sample_orbital_point() (radius,
    then two direction draws via the standard uniform-sphere-point
    construction: cos(theta) uniform in [-1,1], phi uniform in [0, 2*pi)).
    """
    lookup = _get_value_from_lookup_table
    r = lookup(rng.uniform01(), inv_r_table)
    cos_theta = 2.0 * rng.uniform01() - 1.0
    phi = 2.0 * _pi * rng.uniform01()
    sin_theta = _sqrt(1.0 - cos_theta * cos_theta) if cos_theta * cos_theta < 1.0 else 0.0

    x = r * sin_theta * _cos(phi)
    y = r * sin_theta * _sin(phi)
    z = r * cos_theta
    return x, y, z
