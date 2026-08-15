"""Hydrogen atomic orbital math: associated Legendre polynomials, associated
Laguerre radial wavefunction, and their composition into a real orbital
wavefunction psi_real(r, theta, phi).

Ported function-by-function from quantum-physics.js (c) 2020-2022 Manuel
Joffre, www.quantum-physics.polytechnique.fr (see examples/js-calculations/
and tools/orbitals_host/ for the JS reference and the cross-validation
harness used to verify this port against it).

Same algorithms as src/orbitals.h/.cpp (the C++ port), renamed to snake_case
per Python convention, so the two can be compared directly -- this module
exists to let the "MicroPython on ESP32" firmware option be evaluated against
the "C++/ESP-IDF" option before committing to either. Written against the
subset of the language/stdlib available under MicroPython's unix port
(plain lists instead of numpy, only the `math` module), so it runs
unmodified under both MicroPython and CPython 3.

Note on precision: MicroPython's *unix* port (used here for host-side
validation) is commonly built with double-precision floats, as confirmed by
tools/orbitals_host/README.md's float-precision probe. The actual ESP32
MicroPython firmware may default to single-precision floats instead (board
and build dependent) -- verify on the target board/firmware before treating
this module's on-PC precision as representative of what will run on the
device.

Optimized per docs.micropython.org/en/latest/reference/speed_python.html:
module-level aliases for `math` functions (avoids a global-module lookup
plus an attribute lookup on every call -- real cost across the ~1001-sample
loops these functions run in), loop-invariant constants hoisted out of
per-sample loops, and `@micropython.native` on every hot function (compiles
to native machine opcodes instead of bytecode, ~2x per the docs; safe here
since none of these functions raise without an argument or need the
background scheduler, the two documented native-emitter restrictions).
`@micropython.native` is MicroPython-only syntax -- it's a compile-time
pragma the MicroPython compiler recognizes by literal spelling
(`micropython.native`), not a real importable decorator, so there's no
`try/except ImportError` shim that preserves it. This module is no longer
plain-Python-compatible as a result (previously true but never actually
exercised by tools/orbitals_host/, which only ever runs it under real
MicroPython) -- a deliberate tradeoff of that unused property for a real,
measured speedup on the actual target.
"""

import math

_sin = math.sin
_cos = math.cos
_sqrt = math.sqrt
_exp = math.exp
_pow = math.pow
_pi = math.pi

# Maximum principal quantum number supported by laguerre_coeffs()'s
# fixed-length coefficient list. Matches kOrbitalNMax in src/orbitals.h so
# the two ports behave identically for the same inputs.
N_MAX = 16

# Default lookup table resolution, matching kOrbitalTableSize in src/orbitals.h.
TABLE_SIZE = 1001


@micropython.native
def legendre_coeffs(ell, m):
    """Compute the coefficients of the associated Legendre polynomial
    P_l^m(cos theta), expressed as sum_k coeff[k] * u^k * sin(theta)^|m|,
    with u = cos(theta). Normalized so that max(|P_l^m(theta)|) == 1 for
    theta in [0, pi/2). Port of initLegendreCoeffs().

    Args:
        ell: Angular momentum quantum number, ell >= 0.
        m: Magnetic quantum number, -ell <= m <= ell (only |m| affects the
            result -- sign is applied later via the azimuthal factor, see
            psi_real()).

    Returns:
        A list of ell+1 coefficients.
    """
    coeff = [0.0] * (ell + 1)
    abs_m = abs(m)
    ell_ell1 = ell * (ell + 1)
    coeff[0] = 1 - 2 * (ell % 2)

    i_m = ell
    while i_m > abs_m:
        denominator = _sqrt(ell_ell1 - i_m * (i_m - 1))
        k_start = (ell - i_m) % 2
        if k_start == 1:
            coeff[0] = 0
        k = k_start
        while k <= (ell - i_m):
            if k > 0:
                coeff[k - 1] += k * coeff[k] / denominator
            coeff[k + 1] = -(k + 2 * i_m) * coeff[k] / denominator
            k += 2
        i_m -= 1

    # Normalization so that maximum value is equal to one, sampled over
    # theta in [0, pi/2) in steps of pi/100 -- matches initLegendreCoeffs()
    # in quantum-physics.js exactly (same step count/bound). half_pi/step are
    # loop-invariant (theta/2, pi/100 don't change between iterations) --
    # hoisted out instead of recomputing "math.pi / 2" and "math.pi / 100"
    # on each of the ~50 iterations.
    half_pi = _pi / 2
    step = _pi / 100
    max_value = 0.0
    theta = 0.0
    while theta < half_pi:
        value = abs(compute_plm(theta, ell, m, coeff))
        if value > max_value:
            max_value = value
        theta += step
    for i in range(ell + 1):
        coeff[i] /= max_value
    return coeff


@micropython.native
def compute_plm(theta, ell, m, coeff):
    """Evaluate the associated Legendre polynomial P_l^m(theta) from
    precomputed coefficients. Port of computePLM().

    Args:
        theta: Colatitude angle in radians, theta in [0, pi].
        ell: Angular momentum quantum number, must match the value used to
            produce coeff.
        m: Magnetic quantum number, must match the value used to produce
            coeff (only |m| is used).
        coeff: Coefficient list produced by legendre_coeffs(ell, m).

    Returns:
        P_l^m(theta), normalized so its magnitude peaks at 1 over
        theta in [0, pi/2).
    """
    u = _cos(theta)
    abs_m = abs(m)
    total = 0.0
    if (ell - abs_m) % 2 == 0:
        u_pow_j = 1.0
    else:
        u_pow_j = u
    u2 = u * u
    j = (ell - abs_m) % 2
    while j <= ell - abs_m:
        total += coeff[j] * u_pow_j
        u_pow_j *= u2
        j += 2
    return total * _pow(_sin(theta), abs_m)


@micropython.native
def build_legendre_table(ell, m, n=TABLE_SIZE):
    """Build a lookup table of computePLM() sampled at n evenly spaced
    angles covering [0, pi]. Port of initLookupTable().

    Args:
        ell: Angular momentum quantum number, forwarded to legendre_coeffs().
        m: Magnetic quantum number, forwarded to legendre_coeffs().
        n: Number of samples (table resolution).

    Returns:
        A list of n values: table[i] = compute_plm(pi*i/(n-1), ell, m, ...).
    """
    coeff = legendre_coeffs(ell, m)
    table = [0.0] * n
    delta_theta = _pi / (n - 1)  # hoisted: same divide every iteration otherwise
    for i in range(n):
        table[i] = compute_plm(i * delta_theta, ell, m, coeff)
    return table


@micropython.native
def laguerre_coeffs(n, ell):
    """Compute the coefficients of the radial polynomial part of the
    hydrogen radial wavefunction R_{n,ell}(r), i.e. R(r) = (sum_k coeff[k] *
    r^k) * r^ell * exp(-r/n). Port of initLaguerreCoeffs().

    Args:
        n: Principal quantum number, n >= 1 (clamped internally to N_MAX).
        ell: Angular momentum quantum number, 0 <= ell <= n-1 (clamped
            internally).

    Returns:
        A list of N_MAX coefficients (entries at/after index n-ell are zero).
    """
    coeff = [0.0] * N_MAX
    n_clamped = min(n, N_MAX)
    ell_clamped = min(ell, n_clamped - 1)
    ell_clamped = max(ell_clamped, 0)
    degree = n_clamped - ell_clamped

    coeff[0] = 1.0
    k = 0
    while k + 1 < degree:
        coeff[k + 1] = (
            -2 * (1 - (ell_clamped + k + 1.0) / n_clamped) / (k + 1.0) / (2 * ell_clamped + k + 2) * coeff[k]
        )
        k += 1
    return coeff


@micropython.native
def hydrogen_radial_function(r, n, ell, coeff):
    """Evaluate the hydrogen radial wavefunction R_{n,ell}(r) from
    precomputed coefficients. Port of hydrogenRadialFunction().

    Args:
        r: Radial coordinate, r >= 0.
        n: Principal quantum number, must match the value used to produce
            coeff.
        ell: Angular momentum quantum number, must match the value used to
            produce coeff.
        coeff: Coefficient list produced by laguerre_coeffs(n, ell).

    Returns:
        R_{n,ell}(r) (unnormalized in the standard QM sense -- same internal
        normalization convention as the JS reference).
    """
    result = coeff[0]
    p = 1.0
    for k in range(1, n - ell):
        p *= r
        result += p * coeff[k]
    result *= _pow(r, ell) * _exp(-r / n)
    return result


@micropython.native
def build_radial_table(n, ell, table_size=TABLE_SIZE):
    """Build a lookup table of hydrogenRadialFunction() sampled at
    table_size evenly spaced radii covering [0, maxR], with
    maxR = 6*n*n (matches the heuristic in the JS reference: "seems to
    encompass most of the wavefunction"). Port of initLookupTableRadial().

    Args:
        n: Principal quantum number, forwarded to laguerre_coeffs().
        ell: Angular momentum quantum number, forwarded to laguerre_coeffs().
        table_size: Number of samples (table resolution).

    Returns:
        A tuple (table, max_r): table is a list of table_size values,
        table[i] = hydrogen_radial_function(i*max_r/(table_size-1), n, ell, ...);
        max_r is 6*n*n.
    """
    coeff = laguerre_coeffs(n, ell)
    max_r = 6 * n * n
    delta_r = max_r / (table_size - 1)
    table = [0.0] * table_size
    for i in range(table_size):
        r = i * delta_r
        table[i] = hydrogen_radial_function(r, n, ell, coeff)
    return table, max_r


@micropython.native
def get_value_from_lookup_table(x, table):
    """Linear interpolation lookup into a table, mapping x in [0,1] to
    table[0]..table[-1]. Port of getValueFromLookupTable().

    Args:
        x: Normalized position, expected in [0,1] (values below 0 are
            clamped to table[0]; values above 1 return table[-1]).
        table: Table to interpolate into.

    Returns:
        Linearly interpolated value at position x.
    """
    n = len(table)
    i_float = x * (n - 1)
    if i_float < 0:
        i_float = 0.0
    i = int(i_float)
    eta = i_float - i
    if i < n - 1:
        return table[i] * (1 - eta) + table[i + 1] * eta
    return table[n - 1]


@micropython.native
def psi_real(r, theta, phi, n, ell, m, radial_coeff, legendre_coeff):
    """Evaluate the real hydrogen orbital wavefunction psi_{n,l,m}(r, theta,
    phi), composed as R_{n,l}(r) * P_l^m(theta) * (cos(m*phi) for m>=0,
    sin(|m|*phi) for m<0) -- the "Real surface" convention already used for
    angular-only plots in hydrogenOrbitals.js's sphere(). Not a direct port
    (the JS never combines R and P_l^m into a single 3D scalar field), but
    built entirely from the ported primitives above so it can be
    cross-checked end to end.

    Args:
        r: Radial coordinate, r >= 0.
        theta: Colatitude angle in radians, theta in [0, pi].
        phi: Azimuthal angle in radians.
        n: Principal quantum number, must match radial_coeff.
        ell: Angular momentum quantum number, must match both radial_coeff
            and legendre_coeff.
        m: Magnetic quantum number, must match legendre_coeff; its sign
            selects cos (m>=0) vs sin (m<0) for the azimuthal factor.
        radial_coeff: Coefficient list produced by laguerre_coeffs(n, ell).
        legendre_coeff: Coefficient list produced by legendre_coeffs(ell, m).

    Returns:
        psi_{n,l,m}(r, theta, phi), a signed real scalar whose square is
        proportional to probability density.
    """
    r_val = hydrogen_radial_function(r, n, ell, radial_coeff)
    p_val = compute_plm(theta, ell, m, legendre_coeff)
    if m >= 0:
        azimuthal = _cos(m * phi)
    else:
        azimuthal = _sin(-m * phi)
    return r_val * p_val * azimuthal
