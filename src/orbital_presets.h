// Orbital viewer point-cloud model: phase coloring, rank-equalized brightness, point
// turnover ("buzz"/resample), and p90-radius auto-zoom -- everything orbital_view.h needs
// on top of orbital_library.h's compile-time samplers (16 presets, index-matched to
// micropython/cloud_common.py's ORBITAL_PRESETS). Port of cloud_common.py's
// build_point_cloud()/compute_levels()/level_to_rgb()/resample_levels()/
// scale_from_radii().
#pragma once

#include <cstdint>

#include "orbitals.h"    // orb_real_t
#include "pointcloud.h"  // OrbitalPoint, OrbitalSampler, XorShift32

// Static-scratch bound shared by orbital_presets.cpp's computeOrbitalLevels()/
// scaleFromRadii(), and the point-budget ceiling orbital_view.h's N_POINTS must stay
// under -- kept well above the 3000-point device budget cloud_common.N_POINTS already
// uses, as headroom.
constexpr int kOrbitalMaxPoints = 4096;

constexpr int kOrbitalColorMaxLevel = 255; // callers must clamp a resampled point's level to this before comparing

/** Brightness level (0..kOrbitalColorMaxLevel) + wavefunction sign -> this panel's packed
 * RGB565, phase-colored (positive lobe warm, negative lobe cool -- chemistry-diagram
 * convention). Port of cloud_common.level_to_rgb(). */
uint16_t orbitalLevelToColor565(int level, int sign);

/**
 * Rank-based (histogram-equalized) brightness levels: NOT linear min/max, since psi^2 is
 * heavily right-skewed (a handful of samples near the nucleus dominate the range) -- a
 * linear scale would compress most points into the dim end. Sorting and assigning
 * brightness by rank/count spreads every point evenly across the visible range instead.
 * Port of cloud_common.compute_levels().
 *
 * @param psi2          Unnormalized |psi|^2 at each point, length `count`.
 * @param outLevels     [out] length `count`, one brightness value per input point.
 * @param outPsi2Sorted [out] length `count`, psi2 values sorted ascending -- kept as a
 *                      frozen reference so resampleOneOrbitalPoint() can bisect a new
 *                      point's rank without re-sorting the whole cloud.
 */
void computeOrbitalLevels(const orb_real_t* psi2, int count, uint8_t* outLevels, orb_real_t* outPsi2Sorted);

// Point-turnover: fraction of the cloud resampled every kOrbitalCullRefreshFrames frames.
constexpr orb_real_t kOrbitalCullFraction = orb_real_t(0.01);
constexpr int kOrbitalCullRefreshFrames = 3;
// Per-frame flicker fraction (see camera.h's renderPointsColored()'s buzzThreshold param).
constexpr orb_real_t kOrbitalBuzzFraction = orb_real_t(0.40);

/**
 * What resampleOneOrbitalPoint() needs to keep drawing from the same orbital's
 * distribution. `cursor` round-robins through point indices so every point turns over in
 * turn, rather than a random choice leaving some indices stale indefinitely. Port of
 * cloud_common.ResampleState.
 */
struct OrbitalResampleState {
    const OrbitalSampler* sampler;
    // XorShift32 has no default constructor (only an explicit-seed one, see pointcloud.h)
    // -- this default member initializer is what keeps OrbitalResampleState (and
    // OrbitalPresetState, which embeds one) default-constructible, since a member with
    // neither a default constructor nor a default member initializer would otherwise
    // delete the enclosing struct's implicit default constructor. Placeholder seed only;
    // load() always overwrites this from buildOrbitalPointCloud()'s outRng before use.
    XorShift32 rng{1};
    orb_real_t radialCoeff[kOrbitalNMax];
    orb_real_t legendreCoeff[kOrbitalEllMax + 1];
    int n, ell, m;
    orb_real_t psi2Sorted[kOrbitalMaxPoints]; // frozen reference, first `count` entries valid
    int count;
    int cursor = 0;
};

struct ResampledOrbitalPoint {
    int index;
    int level; // can exceed kOrbitalColorMaxLevel, see docstring below -- caller must clamp
    int sign;
};

/**
 * Replace ONE point (round-robin via state->cursor) with a fresh sample from state's
 * orbital distribution, updating `points[idx]` in place. Returns the changed index plus
 * its new brightness level/sign so the caller can re-encode just that one point's color.
 *
 * level can exceed kOrbitalColorMaxLevel when a fresh sample's psi^2 beats everything in
 * the frozen psi2Sorted reference (psi^2 is heavily right-skewed -- see
 * computeOrbitalLevels() -- so eventually happening by chance is expected over a
 * long-running resample, not a bug); callers must clamp before comparing/encoding. Port of
 * one iteration of cloud_common.resample_levels()'s loop.
 */
ResampledOrbitalPoint resampleOneOrbitalPoint(OrbitalResampleState* state, OrbitalPoint* points);

struct OrbitalScale {
    orb_real_t baseScale, zoomAmplitude, rRef;
};

/**
 * Projection scale measured from this preset's own sampled radii: finds the 90th
 * percentile radius and picks a scale so it lands at a fixed target pixel radius. Zoom
 * amplitude is kept proportional to baseScale so breathing never shrinks a preset to a
 * "too far away" dot regardless of its physical size. Port of
 * cloud_common.scale_from_radii().
 */
OrbitalScale scaleFromRadii(const OrbitalPoint* points, int count);

/**
 * Sample `count` points from the (n, ell, m) orbital's probability density (via
 * orbital_library.h's precomputed sampler), plus psi^2 (unnormalized) and sign(psi) at
 * each -- psi2 alone (used for brightness ranking, see computeOrbitalLevels()) loses the
 * sign the instant it's squared, so it's captured separately here for
 * orbitalLevelToColor565()'s phase coloring. Also fills outRadialCoeff/outLegendreCoeff
 * and leaves outRng holding the RNG state AFTER these `count` draws, so
 * OrbitalResampleState can be built directly from this call's outputs to keep resampling
 * from the exact same distribution/stream afterward. Port of cloud_common.build_point_cloud().
 *
 * @param outPsi2/outSigns  [out] scratch only -- caller keeps these just long enough to
 *                          call computeOrbitalLevels() once, unlike outPoints/outRng/coeffs.
 */
void buildOrbitalPointCloud(int n, int ell, int m, OrbitalPoint* outPoints, orb_real_t* outPsi2, int8_t* outSigns,
                             int count, uint32_t seed, XorShift32* outRng, orb_real_t* outRadialCoeff,
                             orb_real_t* outLegendreCoeff);
