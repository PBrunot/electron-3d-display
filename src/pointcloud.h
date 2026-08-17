// Point cloud generation for hydrogen orbitals, built on top of the
// wavefunction engine in orbitals.h. This is the M2 step mentioned in
// CLAUDE.md §5/§7: given a validated psiReal(), draw points (x, y, z)
// distributed according to the probability density
// |psi_{n,l,m}(r,theta,phi)|^2 * r^2 * sin(theta) (the r^2*sin(theta) factor
// is the spherical-coordinates volume element, so points end up distributed
// in *physical* probability, not just where |psi| happens to be large).
//
// Samples r, theta, phi from three INDEPENDENT precomputed inverse-CDF
// (quantile) tables rather than rejection sampling. This is exact, not an
// approximation: the target density factors as
//   |psi|^2 * r^2 * sin(theta) = [r*R(r)]^2 * [P_l^m(theta)^2*sin(theta)] * azimuthal(phi)^2
// i.e. a product of three single-variable functions, so sampling each
// marginal independently and combining the results reproduces the joint
// density exactly (same factorization a prior separable-rejection version
// used -- preserved in git history -- exploited). Each marginal's inverse
// CDF is built once per orbital with a single monotonic forward sweep (no
// per-point search), so sampling a point costs exactly three table lookups
// (interpolated, O(1)) plus the trig to convert to Cartesian -- no rejection
// loop anywhere, so no variance in per-point cost. This refines an earlier
// per-axis *rejection* version (also preserved in git history) the same way
// stef1949/Electron-Orbital-Simulator's GPU sampler does it: precompute the
// inverse function itself, not just the CDF, so there's no search at sample
// time either.
//
// buildInverseCdf()/buildOrbitalSamplerConstexpr() (the table-BUILDING half)
// are constexpr and header-only for the same reason as orbitals.h: a fixed
// set of orbitals' sampler tables can be baked into firmware as compile-time
// .rodata (see main.cpp) instead of rebuilt every boot. sampleOrbitalPoint()
// itself (the per-point RNG draw) stays a regular runtime function in
// pointcloud.cpp -- there's nothing to precompute about a random draw.
//
// Same three-way porting/cross-check discipline as orbitals.h: this is pure
// C++17-syntax-compatible, no Arduino dependency, and uses a small portable
// PRNG (XorShift32) so that the same seed produces the *same* accepted point
// sequence here, in micropython/pointcloud.py, and in
// tools/orbitals_host/js_reference.js -- see tools/orbitals_host/ for the
// cross-check that relies on this.
#pragma once

#include <cstdint>

#include "orbitals.h"

/**
 * Portable xorshift32 pseudo-random generator (Marsaglia's (13,17,5)
 * triple). Not cryptographically secure -- chosen only because it is
 * trivial to port bit-for-bit to MicroPython and JavaScript (a handful of
 * shifts and xors on a 32-bit unsigned word), which is what lets the point
 * clouds produced by all three ports be compared for exact agreement rather
 * than just statistically.
 */
struct XorShift32
{
    uint32_t state;

    /**
     * @param seed  Seed value; 0 is remapped to 1 (xorshift is fixed at the
     *              all-zero state, so 0 would never advance).
     */
    explicit XorShift32(uint32_t seed) : state(seed != 0 ? seed : 1) {}

    /** @return The next raw 32-bit value in the sequence, advancing state. */
    uint32_t next();

    /** @return The next value mapped to [0, 1). */
    orb_real_t uniform01();
};

/** A single point of an orbital's point cloud, in Cartesian coordinates. */
struct OrbitalPoint
{
    orb_real_t x, y, z;
};

/**
 * Precomputed data needed to repeatedly sample a given (n, ell, m) orbital
 * without recomputing anything on every draw. Build once with
 * initOrbitalSampler() or buildOrbitalSamplerConstexpr(), then pass to
 * sampleOrbitalPoint() as many times as needed. n/ell/m are kept only as
 * caller-convenience metadata (e.g. for a UI label) -- sampling itself never
 * needs them again once the tables below are built.
 */
struct OrbitalSampler
{
    int n, ell, m;
    orb_real_t maxR;                             // r ranges over [0, maxR] (= 6*n*n, see orbitals.h).
    orb_real_t invRTable[kOrbitalTableSize];     // Inverse CDF of [r*R(r)]^2: invRTable[k] = r at quantile k/(N-1).
    orb_real_t invThetaTable[kOrbitalTableSize]; // Inverse CDF of P_l^m(theta)^2*sin(theta), same convention.
    orb_real_t invPhiTable[kOrbitalTableSize];   // Inverse CDF of azimuthal(phi)^2, same convention.
};

/**
 * Given non-negative sample weights of a density over [0, domainMax] taken
 * at `count` evenly spaced points, build the inverse CDF: invTable[k] is the
 * x-value at quantile k/(count-1). weight[] is overwritten in place (used as
 * scratch space for the running cumulative sum) -- callers don't need it
 * afterwards. Both the forward cumulative sum and the inverse lookup below
 * are single monotonic sweeps (the CDF is non-decreasing in i, and the
 * target quantile u is non-decreasing in k), so this is O(count) total, not
 * O(count log count) -- no per-point search survives into sampling either,
 * since invTable is later read directly via getValueFromLookupTable().
 */
constexpr void buildInverseCdf(orb_real_t *weight, int count, orb_real_t domainMax, orb_real_t *invTable)
{
    orb_real_t delta = domainMax / orb_real_t(count - 1);

    orb_real_t cumulative = orb_real_t(0);
    for (int i = 0; i < count; i++)
    {
        cumulative += weight[i];
        weight[i] = cumulative; // weight[] now holds the (unnormalized) CDF
    }
    orb_real_t total = weight[count - 1];
    if (total <= orb_real_t(0))
        total = orb_real_t(1); // degenerate guard; shouldn't occur for valid quantum numbers
    for (int i = 0; i < count; i++)
        weight[i] /= total;

    int j = 0;
    for (int k = 0; k < count; k++)
    {
        orb_real_t u = orb_real_t(k) / orb_real_t(count - 1);
        while (j < count - 1 && weight[j] < u)
            j++;
        int j0 = j > 0 ? j - 1 : 0;
        int j1 = j;
        orb_real_t c0 = weight[j0];
        orb_real_t c1 = weight[j1];
        orb_real_t t = (c1 > c0) ? (u - c0) / (c1 - c0) : orb_real_t(0);
        invTable[k] = (orb_real_t(j0) + t * orb_real_t(j1 - j0)) * delta;
    }
}

/**
 * Build an OrbitalSampler for (n, ell, m) and return it by value: the three
 * inverse-CDF tables above from buildRadialTable()'s r*R(r),
 * buildLegendreTable()'s P_l^m(theta), and the azimuthal factor
 * cos(m*phi)/sin(|m|*phi) (evaluated directly, no separate table dependency).
 * Each inverse table is built with a single monotonic forward sweep over the
 * (already monotonic) CDF, so this is O(kOrbitalTableSize) per axis, not
 * O(n log n).
 *
 * constexpr: assign the result to a `constexpr OrbitalSampler` global (see
 * main.cpp) to have the compiler build the whole ~12KB of tables at compile
 * time and embed them as .rodata, instead of spending ~70-80ms doing it at
 * boot (measured on this hardware, see tools/orbitals_host/README.md) --
 * requires a toolchain whose <cmath> supports constexpr sin/cos/pow/exp/sqrt
 * (C++23/26; this project builds with -std=gnu++26, see platformio.ini).
 * Called from a non-constant context (as initOrbitalSampler() below does),
 * this just runs at runtime like any other function -- no different from
 * before.
 *
 * @param n    Principal quantum number.
 * @param ell  Angular momentum quantum number, 0 <= ell <= n-1.
 * @param m    Magnetic quantum number, -ell <= m <= ell.
 * @return     A fully-built OrbitalSampler, ready for sampleOrbitalPoint().
 */
constexpr OrbitalSampler buildOrbitalSamplerConstexpr(int n, int ell, int m)
{
    OrbitalSampler sampler{};
    sampler.n = n;
    sampler.ell = ell;
    sampler.m = m;

    orb_real_t radialCoeff[kOrbitalNMax] = {};
    laguerreCoeffs(n, ell, radialCoeff);
    orb_real_t legendreCoeff[kOrbitalEllMax + 1] = {};
    legendreCoeffs(ell, m, legendreCoeff);

    orb_real_t maxR = orb_real_t(6 * n * n);
    sampler.maxR = maxR;

    {
        orb_real_t weight[kOrbitalTableSize] = {};
        orb_real_t deltaR = maxR / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++)
        {
            orb_real_t r = orb_real_t(i) * deltaR;
            orb_real_t R = hydrogenRadialFunction(r, n, ell, radialCoeff);
            weight[i] = (r * R) * (r * R);
        }
        buildInverseCdf(weight, kOrbitalTableSize, maxR, sampler.invRTable);
    }
    {
        orb_real_t weight[kOrbitalTableSize] = {};
        orb_real_t deltaTheta = kOrbitalPi / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++)
        {
            orb_real_t theta = orb_real_t(i) * deltaTheta;
            orb_real_t P = computePLM(theta, ell, m, legendreCoeff);
            weight[i] = P * P * std::sin(theta);
        }
        buildInverseCdf(weight, kOrbitalTableSize, kOrbitalPi, sampler.invThetaTable);
    }
    {
        orb_real_t weight[kOrbitalTableSize] = {};
        orb_real_t twoPi = orb_real_t(2) * kOrbitalPi;
        orb_real_t deltaPhi = twoPi / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++)
        {
            orb_real_t phi = orb_real_t(i) * deltaPhi;
            orb_real_t azimuthal = (m >= 0) ? std::cos(orb_real_t(m) * phi) : std::sin(orb_real_t(-m) * phi);
            weight[i] = azimuthal * azimuthal; // m==0 -> constant 1, degenerates to a uniform phi distribution
        }
        buildInverseCdf(weight, kOrbitalTableSize, twoPi, sampler.invPhiTable);
    }

    return sampler;
}

/**
 * Runtime-friendly wrapper around buildOrbitalSamplerConstexpr() for callers
 * that want an output-pointer style (e.g. tools/orbitals_host/gen_points_c.cpp,
 * kept unchanged for the cross-validation harness). Always runs at runtime
 * (there is no benefit to a compile-time result here since *sampler is a
 * runtime object) -- for compile-time-embedded tables, call
 * buildOrbitalSamplerConstexpr() directly from a constexpr context instead.
 *
 * @param sampler  [out] Sampler state to initialize.
 * @param n        Principal quantum number.
 * @param ell      Angular momentum quantum number, 0 <= ell <= n-1.
 * @param m        Magnetic quantum number, -ell <= m <= ell.
 */
inline void initOrbitalSampler(OrbitalSampler *sampler, int n, int ell, int m)
{
    *sampler = buildOrbitalSamplerConstexpr(n, ell, m);
}

/**
 * Draw one point from the (r, theta, phi) probability density
 * |psi_{n,l,m}|^2 * r^2 * sin(theta) via inverse-CDF (quantile) sampling:
 * one uniform draw per axis, mapped through that axis's precomputed inverse
 * table via linear interpolation (getValueFromLookupTable()). Always
 * exactly 3 RNG draws per point, in the fixed order (r, theta, phi) --
 * mirrored exactly in micropython/pointcloud.py and
 * tools/orbitals_host/js_reference.js so the same seed produces the same
 * point.
 *
 * @param sampler  Sampler state produced by initOrbitalSampler() or
 *                 buildOrbitalSamplerConstexpr().
 * @param rng      PRNG state, advanced by this call.
 * @return          A point in Cartesian coordinates.
 */
OrbitalPoint sampleOrbitalPoint(const OrbitalSampler *sampler, XorShift32 *rng);

// --- Angular/radial split, for multi-element atom point clouds (see atom_cloud.h) ---
//
// A hydrogenic wavefunction's ANGULAR part (theta/phi) never depends on nuclear charge --
// only its RADIAL extent does, via the r -> Z_eff*r substitution (see slater.h). So unlike
// buildOrbitalSamplerConstexpr() above (one full n/ell/m/Z_eff=1 sampler, fine for a fixed
// hydrogen library), a multi-element atom viewer needs the angular tables (keyed only by
// (ell, m), so still compile-time-embeddable -- there are just 16 of them for ell<=3, see
// angular_library.h) kept SEPARATE from the radial table (keyed by (n, ell, Z_eff), built at
// RUNTIME once per selected element/subshell -- there are too many (n,ell,Z_eff)
// combinations across 118 elements to embed them all).

/** Angular-only tables for a given (ell, m), independent of n and Z_eff. */
struct OrbitalAngularTables
{
    int ell, m;
    orb_real_t invThetaTable[kOrbitalTableSize];
    orb_real_t invPhiTable[kOrbitalTableSize];
};

/**
 * Build the angular tables for (ell, m) -- constexpr/compile-time-embeddable, see
 * angular_library.h for the full ell<=3 set assigned to a `constexpr` global.
 */
constexpr OrbitalAngularTables buildAngularTablesConstexpr(int ell, int m)
{
    OrbitalAngularTables t{};
    t.ell = ell;
    t.m = m;

    orb_real_t legendreCoeff[kOrbitalEllMax + 1] = {};
    legendreCoeffs(ell, m, legendreCoeff);

    {
        orb_real_t weight[kOrbitalTableSize] = {};
        orb_real_t deltaTheta = kOrbitalPi / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++)
        {
            orb_real_t theta = orb_real_t(i) * deltaTheta;
            orb_real_t P = computePLM(theta, ell, m, legendreCoeff);
            weight[i] = P * P * std::sin(theta);
        }
        buildInverseCdf(weight, kOrbitalTableSize, kOrbitalPi, t.invThetaTable);
    }
    {
        orb_real_t weight[kOrbitalTableSize] = {};
        orb_real_t twoPi = orb_real_t(2) * kOrbitalPi;
        orb_real_t deltaPhi = twoPi / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++)
        {
            orb_real_t phi = orb_real_t(i) * deltaPhi;
            orb_real_t azimuthal = (m >= 0) ? std::cos(orb_real_t(m) * phi) : std::sin(orb_real_t(-m) * phi);
            weight[i] = azimuthal * azimuthal;
        }
        buildInverseCdf(weight, kOrbitalTableSize, twoPi, t.invPhiTable);
    }
    return t;
}

/** Radial-only table for a given (n, ell, zEff), built at runtime (see block comment above). */
struct RadialTable
{
    orb_real_t maxR;
    orb_real_t invRTable[kOrbitalTableSize];
};

/**
 * Build the radial table for subshell (n, ell) at effective nuclear charge zEff (1.0 =
 * plain hydrogen). Runtime only -- zEff comes from a runtime element selection (slater.h),
 * so this can't be a compile-time constant the way the angular tables are. Same r -> zEff*r
 * substitution and max_r = 6*n*n/zEff shrink as micropython/pointcloud.py's
 * init_orbital_sampler()/init_radial_sampler().
 *
 * Not reentrant/thread-safe (see the static scratch buffer below) -- fine for
 * atom_cloud.h's sequential per-group loop, not safe to call from multiple tasks/ISRs.
 */
inline RadialTable buildRadialSamplerRuntime(int n, int ell, orb_real_t zEff)
{
    RadialTable rt{};
    orb_real_t radialCoeff[kOrbitalNMax] = {};
    laguerreCoeffs(n, ell, radialCoeff);

    orb_real_t maxR = orb_real_t(6 * n * n) / zEff;
    rt.maxR = maxR;
    // static, not a stack array: 1001 floats (~4KB) is a large chunk to put on a task
    // stack (this is what actually overflowed app_main's default-sized stack when it was
    // a local array here -- unlike orbitals.h/pointcloud.h's constexpr table builders,
    // THIS function genuinely runs on the target's real stack at runtime).
    static orb_real_t weight[kOrbitalTableSize];
    orb_real_t deltaR = maxR / orb_real_t(kOrbitalTableSize - 1);
    for (int i = 0; i < kOrbitalTableSize; i++)
    {
        orb_real_t r = orb_real_t(i) * deltaR;
        orb_real_t R = hydrogenRadialFunction(zEff * r, n, ell, radialCoeff);
        weight[i] = (r * R) * (r * R);
    }
    buildInverseCdf(weight, kOrbitalTableSize, maxR, rt.invRTable);
    return rt;
}

/**
 * Draw one point from a specific real orbital (n, ell, m) given its precomputed radial
 * (Z_eff-scaled) and angular (Z_eff-independent) tables. Same 3-draws-per-point order as
 * sampleOrbitalPoint() -- used for a partially-filled subshell's individually-occupied
 * orbitals (Hund's rule groups, see atom_cloud.h).
 */
inline OrbitalPoint sampleOrientedPoint(const RadialTable &radial, const OrbitalAngularTables &angular,
                                        XorShift32 *rng)
{
    orb_real_t r = getValueFromLookupTable(rng->uniform01(), radial.invRTable, kOrbitalTableSize);
    orb_real_t theta = getValueFromLookupTable(rng->uniform01(), angular.invThetaTable, kOrbitalTableSize);
    orb_real_t phi = getValueFromLookupTable(rng->uniform01(), angular.invPhiTable, kOrbitalTableSize);

    orb_real_t sinTheta = std::sin(theta);
    OrbitalPoint pt;
    pt.x = r * sinTheta * std::cos(phi);
    pt.y = r * sinTheta * std::sin(phi);
    pt.z = r * std::cos(theta);
    return pt;
}

/**
 * Draw one point from a spherically-symmetric density whose radial part is `radial` and
 * whose angular part is uniform over the sphere -- the spherically-averaged-subshell
 * approximation used for FULL subshells (exact, by Unsoeld's theorem: summing |Y_l^m|^2
 * over every m in a full subshell gives a constant). Port of
 * micropython/pointcloud.py's sample_isotropic_point().
 */
inline OrbitalPoint sampleIsotropicPoint(const RadialTable &radial, XorShift32 *rng)
{
    orb_real_t r = getValueFromLookupTable(rng->uniform01(), radial.invRTable, kOrbitalTableSize);
    orb_real_t cosTheta = orb_real_t(2) * rng->uniform01() - orb_real_t(1);
    orb_real_t phi = orb_real_t(2) * kOrbitalPi * rng->uniform01();
    orb_real_t cosTheta2 = cosTheta * cosTheta;
    orb_real_t sinTheta = (cosTheta2 < orb_real_t(1)) ? std::sqrt(orb_real_t(1) - cosTheta2) : orb_real_t(0);

    OrbitalPoint pt;
    pt.x = r * sinTheta * std::cos(phi);
    pt.y = r * sinTheta * std::sin(phi);
    pt.z = r * cosTheta;
    return pt;
}
