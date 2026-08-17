// Hydrogen atomic orbital math: associated Legendre polynomials, associated
// Laguerre radial wavefunction, and their composition into a real orbital
// wavefunction psiReal(r, theta, phi).
//
// Ported function-by-function from quantum-physics.js (c) 2020-2022 Manuel
// Joffre, www.quantum-physics.polytechnique.fr (see
// examples/js-calculations/ and tools/orbitals_host/ for the JS reference and
// the cross-validation harness used to verify this port against it).
//
// Header-only and constexpr: every function below can be evaluated by the
// compiler at compile time, given compile-time-constant arguments (e.g. a
// `constexpr OrbitalSampler` built in pointcloud.h). This lets the coefficient
// and sampling tables for a fixed set of orbitals be baked into the firmware
// as .rodata instead of recomputed every boot (see CLAUDE.md's "Correzione
// verificata"/M2 notes on why this project avoids on-device recomputation).
// `constexpr` here does not *force* compile-time evaluation -- called from a
// non-constant context (as tools/orbitals_host's generators do, compiling
// against -std=c++17 where <cmath> isn't constexpr) these just behave as
// ordinary runtime functions, so the existing cross-validation harness is
// unaffected. orbitals.cpp is kept only so build scripts that list it as a
// translation unit keep working; it has no content of its own anymore.
//
// Pure C++17-syntax-compatible (constexpr requires C++23/26 <cmath> support
// from the toolchain to actually fold at compile time -- see main.cpp), no
// Arduino/platform dependency, so this compiles both natively on a PC (see
// tools/orbitals_host/) and under PlatformIO for the ESP32.
#pragma once

#include <algorithm>
#include <cmath>
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

// Pi, at the working precision, shared by orbitals.h and pointcloud.h.
constexpr orb_real_t kOrbitalPi = orb_real_t(3.14159265358979323846);

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
constexpr void legendreCoeffs(int ell, int m, orb_real_t *coeff);

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
constexpr orb_real_t computePLM(orb_real_t theta, int ell, int m, const orb_real_t *coeff)
{
    orb_real_t u = std::cos(theta);
    int absM = m < 0 ? -m : m;
    orb_real_t sum = orb_real_t(0);
    orb_real_t uPowJ;
    if ((ell - absM) % 2 == 0)
        uPowJ = orb_real_t(1);
    else
        uPowJ = u;
    orb_real_t u2 = u * u;
    for (int j = (ell - absM) % 2; j <= ell - absM; j += 2, uPowJ *= u2)
        sum += coeff[j] * uPowJ;
    return sum * std::pow(std::sin(theta), orb_real_t(absM));
}

constexpr void legendreCoeffs(int ell, int m, orb_real_t *coeff)
{
    for (int i = 0; i <= ell; i++)
        coeff[i] = orb_real_t(0);

    int absM = m < 0 ? -m : m;
    int ellEll1 = ell * (ell + 1);
    coeff[0] = orb_real_t(1 - 2 * (ell % 2));

    for (int iM = ell; iM > absM; iM--)
    {
        orb_real_t denominator = std::sqrt(orb_real_t(ellEll1 - iM * (iM - 1)));
        int kStart = (ell - iM) % 2;
        if (kStart == 1)
            coeff[0] = orb_real_t(0);
        for (int k = kStart; k <= (ell - iM); k += 2)
        {
            if (k > 0)
                coeff[k - 1] += orb_real_t(k) * coeff[k] / denominator;
            coeff[k + 1] = -orb_real_t(k + 2 * iM) * coeff[k] / denominator;
        }
    }

    // Normalization so that maximum value is equal to one, sampled over
    // theta in [0, pi/2) in steps of pi/100 -- matches initLegendreCoeffs()
    // in quantum-physics.js exactly (same step count/bound), so the two
    // implementations converge to the same normalization constant.
    orb_real_t maxValue = orb_real_t(0);
    for (orb_real_t theta = orb_real_t(0); theta < kOrbitalPi / 2; theta += kOrbitalPi / 100)
    {
        orb_real_t value = computePLM(theta, ell, m, coeff);
        maxValue = std::max(maxValue, std::abs(value));
    }
    for (int iM = 0; iM <= ell; iM++)
        coeff[iM] /= maxValue;
}

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
constexpr void buildLegendreTable(int ell, int m, orb_real_t *table, int n = kOrbitalTableSize)
{
    orb_real_t coeff[kOrbitalEllMax + 1] = {};
    legendreCoeffs(ell, m, coeff);
    for (int i = 0; i < n; i++)
    {
        orb_real_t theta = kOrbitalPi * orb_real_t(i) / orb_real_t(n - 1);
        table[i] = computePLM(theta, ell, m, coeff);
    }
}

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
constexpr void laguerreCoeffs(int n, int ell, orb_real_t *coeff)
{
    for (int k = 0; k < kOrbitalNMax; k++)
        coeff[k] = orb_real_t(0);

    int nClamped = n < kOrbitalNMax ? n : kOrbitalNMax;
    int ellClamped = ell < nClamped - 1 ? ell : nClamped - 1;
    if (ellClamped < 0)
        ellClamped = 0;
    int degree = nClamped - ellClamped;

    coeff[0] = orb_real_t(1);
    for (int k = 0; k + 1 < degree; k++)
    {
        orb_real_t kk = orb_real_t(k);
        coeff[k + 1] = -orb_real_t(2) * (orb_real_t(1) - (orb_real_t(ellClamped) + kk + orb_real_t(1)) / orb_real_t(nClamped)) /
                       (kk + orb_real_t(1)) / orb_real_t(2 * ellClamped + k + 2) * coeff[k];
    }
}

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
constexpr orb_real_t hydrogenRadialFunction(orb_real_t r, int n, int ell, const orb_real_t *coeff)
{
    orb_real_t result = coeff[0];
    orb_real_t p = orb_real_t(1);
    for (int k = 1; k < n - ell; k++)
    {
        p = p * r;
        result += p * coeff[k];
    }
    result *= std::pow(r, orb_real_t(ell)) * std::exp(-r / orb_real_t(n));
    return result;
}

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
constexpr void buildRadialTable(int n, int ell, orb_real_t *table, int tableSize = kOrbitalTableSize,
                                orb_real_t *maxROut = nullptr)
{
    orb_real_t coeff[kOrbitalNMax] = {};
    laguerreCoeffs(n, ell, coeff);
    orb_real_t maxR = orb_real_t(6 * n * n);
    orb_real_t deltaR = maxR / orb_real_t(tableSize - 1);
    for (int i = 0; i < tableSize; i++)
    {
        orb_real_t r = orb_real_t(i) * deltaR;
        table[i] = hydrogenRadialFunction(r, n, ell, coeff);
    }
    if (maxROut)
        *maxROut = maxR;
}

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
constexpr orb_real_t getValueFromLookupTable(orb_real_t x, const orb_real_t *table, int n)
{
    orb_real_t iFloat = x * orb_real_t(n - 1);
    iFloat = std::max(orb_real_t(0), iFloat);
    int i = int(iFloat); // truncation toward zero == floor() here since iFloat >= 0
    orb_real_t eta = iFloat - orb_real_t(i);
    if (i < n - 1)
        return table[i] * (orb_real_t(1) - eta) + table[i + 1] * eta;
    else
        return table[n - 1];
}

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
constexpr orb_real_t psiReal(orb_real_t r, orb_real_t theta, orb_real_t phi, int n, int ell, int m,
                             const orb_real_t *radialCoeff, const orb_real_t *legendreCoeffArr)
{
    orb_real_t R = hydrogenRadialFunction(r, n, ell, radialCoeff);
    orb_real_t P = computePLM(theta, ell, m, legendreCoeffArr);
    orb_real_t azimuthal = (m >= 0) ? std::cos(orb_real_t(m) * phi) : std::sin(orb_real_t(-m) * phi);
    return R * P * azimuthal;
}
