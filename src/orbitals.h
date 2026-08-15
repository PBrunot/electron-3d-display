// Hydrogen atomic orbital math: associated Legendre polynomials, associated
// Laguerre radial wavefunction, and their composition into a real orbital
// wavefunction psiReal(r, theta, phi).
//
// Ported function-by-function from quantum-physics.js (c) 2020-2022 Manuel
// Joffre, www.quantum-physics.polytechnique.fr (see
// examples/js-calculations/ and tools/orbitals_host/ for the JS reference and
// the cross-validation harness used to verify this port against it).
//
// Pure C++17, no Arduino/platform dependency, so this compiles both natively
// on a PC (see tools/orbitals_host/) and under PlatformIO for the ESP32.
#pragma once

#include <cstddef>

#if defined(ORBITAL_USE_DOUBLE)
using orb_real_t = double;
#else
using orb_real_t = float; // default: matches the eventual ESP32 build
#endif

// Maximum principal quantum number / angular momentum supported by the fixed-size
// coefficient arrays below. Mirrors nMax/ellMax in quantum-physics.js, but kept
// much smaller here since this project only ever needs modest (n, ell) values.
constexpr int kOrbitalNMax = 16;
constexpr int kOrbitalEllMax = kOrbitalNMax - 1;

// Default lookup table resolution, matching nTable/nTableRadial in the JS reference.
constexpr int kOrbitalTableSize = 1001;

/**
 * Compute the coefficients of the associated Legendre polynomial P_l^m(cos
 * theta), expressed as sum_k coeff[k] * u^k * sin(theta)^|m|, with
 * u = cos(theta). Normalized so that max(|P_l^m(theta)|) == 1 for theta in
 * [0, pi/2). Port of initLegendreCoeffs().
 *
 * @param ell    Angular momentum quantum number ell >= 0.
 * @param m      Magnetic quantum number, -ell <= m <= ell (only |m| affects
 *               the result -- sign is applied later via the azimuthal
 *               factor, see psiReal()).
 * @param coeff  [out] Coefficient array, must hold at least ell+1 entries.
 *               Entries with index > ell-|m| or of the wrong parity are
 *               zeroed but otherwise unused by computePLM().
 */
void legendreCoeffs(int ell, int m, orb_real_t* coeff);

/**
 * Evaluate the associated Legendre polynomial P_l^m(theta) from
 * precomputed coefficients. Port of computePLM().
 *
 * @param theta  Colatitude angle in radians, theta in [0, pi].
 * @param ell    Angular momentum quantum number, must match the value
 *               passed to legendreCoeffs() when coeff was produced.
 * @param m      Magnetic quantum number, must match the value passed to
 *               legendreCoeffs() when coeff was produced (only |m| is used).
 * @param coeff  Coefficient array produced by legendreCoeffs(ell, m, ...).
 * @return       P_l^m(theta), normalized so its magnitude peaks at 1 over
 *               theta in [0, pi/2).
 */
orb_real_t computePLM(orb_real_t theta, int ell, int m, const orb_real_t* coeff);

/**
 * Fill a lookup table with computePLM() sampled at n evenly spaced angles
 * covering [0, pi]. Port of initLookupTable().
 *
 * @param ell    Angular momentum quantum number, forwarded to legendreCoeffs().
 * @param m      Magnetic quantum number, forwarded to legendreCoeffs().
 * @param table  [out] Table array, must hold at least n entries.
 *               table[i] = computePLM(pi*i/(n-1), ell, m, ...) for i in [0, n).
 * @param n      Number of samples (table resolution). Defaults to
 *               kOrbitalTableSize.
 */
void buildLegendreTable(int ell, int m, orb_real_t* table, int n = kOrbitalTableSize);

/**
 * Compute the coefficients of the radial polynomial part of the hydrogen
 * radial wavefunction R_{n,ell}(r), i.e. R(r) = (sum_k coeff[k] * r^k) *
 * r^ell * exp(-r/n). Port of initLaguerreCoeffs().
 *
 * @param n      Principal quantum number, n >= 1 (clamped internally to
 *               kOrbitalNMax).
 * @param ell    Angular momentum quantum number, 0 <= ell <= n-1 (clamped
 *               internally).
 * @param coeff  [out] Coefficient array, must hold at least n-ell entries
 *               (entries beyond that, up to kOrbitalNMax, are zeroed).
 */
void laguerreCoeffs(int n, int ell, orb_real_t* coeff);

/**
 * Evaluate the hydrogen radial wavefunction R_{n,ell}(r) from precomputed
 * coefficients. Port of hydrogenRadialFunction().
 *
 * @param r      Radial coordinate, r >= 0.
 * @param n      Principal quantum number, must match the value passed to
 *               laguerreCoeffs() when coeff was produced.
 * @param ell    Angular momentum quantum number, must match the value
 *               passed to laguerreCoeffs() when coeff was produced.
 * @param coeff  Coefficient array produced by laguerreCoeffs(n, ell, ...).
 * @return       R_{n,ell}(r) (unnormalized in the standard QM sense -- same
 *               internal normalization convention as the JS reference).
 */
orb_real_t hydrogenRadialFunction(orb_real_t r, int n, int ell, const orb_real_t* coeff);

/**
 * Fill a lookup table with hydrogenRadialFunction() sampled at tableSize
 * evenly spaced radii covering [0, maxR], with maxR = 6*n*n (matches the
 * heuristic in the JS reference: "seems to encompass most of the
 * wavefunction"). Port of initLookupTableRadial().
 *
 * @param n          Principal quantum number, forwarded to laguerreCoeffs().
 * @param ell        Angular momentum quantum number, forwarded to laguerreCoeffs().
 * @param table      [out] Table array, must hold at least tableSize entries.
 *                   table[i] = hydrogenRadialFunction(i*maxR/(tableSize-1), n, ell, ...).
 * @param tableSize  Number of samples (table resolution). Defaults to
 *                   kOrbitalTableSize.
 * @param maxROut    [out, optional] If non-null, receives maxR = 6*n*n.
 */
void buildRadialTable(int n, int ell, orb_real_t* table, int tableSize = kOrbitalTableSize,
                       orb_real_t* maxROut = nullptr);

/**
 * Linear interpolation lookup into a table, mapping x in [0,1] to
 * table[0]..table[n-1]. Port of getValueFromLookupTable().
 *
 * @param x      Normalized position, expected in [0,1] (values below 0 are
 *               clamped to table[0]; values above 1 return table[n-1]).
 * @param table  Table to interpolate into.
 * @param n      Number of entries in table.
 * @return       Linearly interpolated value at position x.
 */
orb_real_t getValueFromLookupTable(orb_real_t x, const orb_real_t* table, int n);

/**
 * Evaluate the real hydrogen orbital wavefunction psi_{n,l,m}(r, theta,
 * phi), composed as R_{n,l}(r) * P_l^m(theta) * (cos(m*phi) for m>=0,
 * sin(|m|*phi) for m<0) -- the "Real surface" convention already used for
 * angular-only plots in hydrogenOrbitals.js's sphere(). Not a direct port
 * (the JS never combines R and P_l^m into a single 3D scalar field), but
 * built entirely from the ported primitives above so it can be
 * cross-checked end to end.
 *
 * @param r                Radial coordinate, r >= 0.
 * @param theta            Colatitude angle in radians, theta in [0, pi].
 * @param phi              Azimuthal angle in radians.
 * @param n                Principal quantum number, must match radialCoeff.
 * @param ell              Angular momentum quantum number, must match both
 *                         radialCoeff and legendreCoeffArr.
 * @param m                Magnetic quantum number, must match legendreCoeffArr;
 *                         its sign selects cos (m>=0) vs sin (m<0) for the
 *                         azimuthal factor.
 * @param radialCoeff      Coefficient array produced by laguerreCoeffs(n, ell, ...).
 * @param legendreCoeffArr Coefficient array produced by legendreCoeffs(ell, m, ...).
 * @return                 psi_{n,l,m}(r, theta, phi), a signed real scalar
 *                         whose square is proportional to probability density.
 */
orb_real_t psiReal(orb_real_t r, orb_real_t theta, orb_real_t phi, int n, int ell, int m,
                    const orb_real_t* radialCoeff, const orb_real_t* legendreCoeffArr);
