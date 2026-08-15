// Rejection-sampled point cloud generation for hydrogen orbitals, built on
// top of the wavefunction engine in orbitals.h. This is the M2 step
// mentioned in CLAUDE.md §5/§7: given a validated psiReal(), draw points
// (x, y, z) distributed according to the probability density
// |psi_{n,l,m}(r,theta,phi)|^2 * r^2 * sin(theta) (the r^2*sin(theta) factor
// is the spherical-coordinates volume element, so points end up distributed
// in *physical* probability, not just where |psi| happens to be large).
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
 * Precomputed data needed to repeatedly rejection-sample a given (n, ell, m)
 * orbital without recomputing coefficients/tables on every draw. Build once
 * with initOrbitalSampler(), then pass to sampleOrbitalPoint() as many times
 * as needed.
 */
struct OrbitalSampler {
    int n, ell, m;
    orb_real_t radialCoeff[kOrbitalNMax];
    orb_real_t legendreCoeff[kOrbitalEllMax + 1];
    orb_real_t maxR;         // r is sampled uniformly in [0, maxR] (= 6*n*n, see orbitals.h).
    orb_real_t densityBound; // Upper bound on |psi*r|^2 * sin(theta) over the whole domain.
};

/**
 * Precompute an OrbitalSampler for (n, ell, m): coefficients (via
 * laguerreCoeffs()/legendreCoeffs()) plus a valid upper bound on the target
 * density, obtained from the peak magnitudes of buildRadialTable()'s
 * r*R(r) and buildLegendreTable()'s P_l^m(theta) (each padded by a small
 * safety margin to guard against the true continuous peak falling between
 * two tabulated points).
 *
 * @param sampler  [out] Sampler state to initialize.
 * @param n        Principal quantum number.
 * @param ell      Angular momentum quantum number, 0 <= ell <= n-1.
 * @param m        Magnetic quantum number, -ell <= m <= ell.
 */
void initOrbitalSampler(OrbitalSampler* sampler, int n, int ell, int m);

/**
 * Draw one point from the (r, theta, phi) probability density
 * |psi_{n,l,m}|^2 * r^2 * sin(theta) via rejection sampling: repeatedly draw
 * a uniform random (r, theta, phi) candidate and a uniform random threshold
 * in [0, sampler->densityBound], accepting the candidate once the threshold
 * falls under its actual density. Number of RNG draws per accepted point is
 * variable (that's the nature of rejection sampling) but bounded by
 * maxAttempts.
 *
 * @param sampler      Sampler state produced by initOrbitalSampler().
 * @param rng          PRNG state, advanced by this call.
 * @param maxAttempts  Safety cap on rejection-loop iterations; if exceeded
 *                     (which should not happen for a correctly computed
 *                     densityBound), returns the origin (0,0,0).
 * @return              An accepted point in Cartesian coordinates.
 */
OrbitalPoint sampleOrbitalPoint(const OrbitalSampler* sampler, XorShift32* rng, int maxAttempts = 100000);
