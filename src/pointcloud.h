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
// Same three-way porting/cross-check discipline as orbitals.h: this is pure
// C++17, no Arduino dependency, and uses a small portable PRNG (XorShift32)
// so that the same seed produces the *same* accepted point sequence here,
// in micropython/pointcloud.py, and in tools/orbitals_host/js_reference.js
// -- see tools/orbitals_host/ for the cross-check that relies on this.
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
struct XorShift32 {
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
struct OrbitalPoint {
    orb_real_t x, y, z;
};

/**
 * Precomputed data needed to repeatedly sample a given (n, ell, m) orbital
 * without recomputing anything on every draw. Build once with
 * initOrbitalSampler(), then pass to sampleOrbitalPoint() as many times as
 * needed. n/ell/m are kept only as caller-convenience metadata (e.g. for a
 * UI label) -- sampling itself never needs them again once the tables below
 * are built.
 */
struct OrbitalSampler {
    int n, ell, m;
    orb_real_t maxR;                             // r ranges over [0, maxR] (= 6*n*n, see orbitals.h).
    orb_real_t invRTable[kOrbitalTableSize];      // Inverse CDF of [r*R(r)]^2: invRTable[k] = r at quantile k/(N-1).
    orb_real_t invThetaTable[kOrbitalTableSize];  // Inverse CDF of P_l^m(theta)^2*sin(theta), same convention.
    orb_real_t invPhiTable[kOrbitalTableSize];    // Inverse CDF of azimuthal(phi)^2, same convention.
};

/**
 * Precompute an OrbitalSampler for (n, ell, m): builds the three inverse-CDF
 * tables above from buildRadialTable()'s r*R(r), buildLegendreTable()'s
 * P_l^m(theta), and the azimuthal factor cos(m*phi)/sin(|m|*phi) (evaluated
 * directly, no separate table dependency). Each inverse table is built with
 * a single monotonic forward sweep over the (already monotonic) CDF, so
 * this is O(kOrbitalTableSize) per axis, not O(n log n).
 *
 * @param sampler  [out] Sampler state to initialize.
 * @param n        Principal quantum number.
 * @param ell      Angular momentum quantum number, 0 <= ell <= n-1.
 * @param m        Magnetic quantum number, -ell <= m <= ell.
 */
void initOrbitalSampler(OrbitalSampler* sampler, int n, int ell, int m);

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
 * @param sampler  Sampler state produced by initOrbitalSampler().
 * @param rng      PRNG state, advanced by this call.
 * @return          A point in Cartesian coordinates.
 */
OrbitalPoint sampleOrbitalPoint(const OrbitalSampler* sampler, XorShift32* rng);
