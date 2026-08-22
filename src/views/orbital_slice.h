/**
 * @file orbital_slice.h
 * @brief Static 2D plane-slice heatmap of the current hydrogen orbital's |psi| -- physics
 *        table build + per-frame draw only. See SLICE.md for the plane-azimuth derivation;
 *        the gesture sequence (intro card, fade in/hold/fade out, abort-on-movement) is
 *        orchestrated by orbital_view.cpp's runSliceSequence(), the same split atom_view.cpp
 *        uses between its own physics/render helpers and runDissectionSequence().
 */
#pragma once

#include <cstdint>

class Display;

#include "physics/orbitals.h"        // orb_real_t
#include "config/visual_constants.h" // kSliceGridSize

/**
 * @brief One built slice: the (n, ell, m) it was built for, the static lobe-plane azimuth and
 *        grid framing, and the per-cell density brightness the heatmap is drawn from every frame.
 *
 * The plane is fixed at build time (D2, SLICE.md section 3/4) -- there is no per-frame trig,
 * so every frame of a sequence just re-reads level[] at a different fade.
 */
struct SliceTable
{
    int n, ell, m;
    orb_real_t planeAzimuth; ///< Lobe-plane ph0: 0 for m >= 0, pi/(2|m|) for m < 0.
    orb_real_t extentBohr;   ///< Grid half-extent, kSliceFramingFactor * rRef, in Bohr radii.
    orb_real_t extentPm;     ///< Same, in picometers -- feeds drawScaleBar().
    /// Density-normalized brightness, 0..255: 255*min(1,|psi|^2/v99)^gamma, gamma computed
    /// per orbital by buildSliceTable()'s auto-exposure pass (see visual_constants.h's
    /// kSliceExposureScale). Rendered through kSliceColormapStops -- a sequential ramp, so the
    /// slice reads as a density heatmap rather than a flattened copy of the phase-colored 3D
    /// cloud (no sign channel; see the colormap's comment).
    uint8_t level[kSliceGridSize * kSliceGridSize];
};

/**
 * @brief Sequential density colormap for the slice heatmap -- an approximation of seaborn's
 *        "rocket" ramp (near-black -> deep purple -> magenta -> coral -> pale yellow-cream),
 *        the same perceptual family the reference repo (hydrogen-wavefunctions) renders its
 *        |psi|^2 slices with. 16 stops, linearly interpolated per channel at render time;
 *        tune by eye here. Density-only by design: the slice deliberately drops the 3D
 *        cloud's phase coloring (sign) so it reads as a heatmap, not as the same orange/blue
 *        blob language as the cloud.
 */
struct SliceColorStop
{
    uint8_t r, g, b;
};
inline constexpr SliceColorStop kSliceColormapStops[] = {
    {3, 0, 5},       // near-black
    {16, 2, 22},     // dark violet
    {35, 4, 48},     // deep purple
    {56, 7, 78},     //
    {78, 12, 105},   //
    {102, 18, 122},  // purple
    {127, 26, 130},  // magenta-purple
    {152, 37, 129},  //
    {178, 52, 118},  // magenta
    {203, 71, 100},  // pink-red
    {224, 96, 80},   // coral
    {238, 126, 64},  // orange
    {247, 160, 58},  // amber
    {251, 198, 66},  // yellow
    {252, 230, 112}, // pale yellow
    {252, 253, 191}  // cream
};
inline constexpr int kSliceColormapStopsCount = int(sizeof(kSliceColormapStops) / sizeof(kSliceColormapStops[0]));

/**
 * @brief One-time build: sample base = R(r)*P(theta) at every cell center, fold in the
 *        per-half azimuthal sign flip (see SLICE.md section 3), then density-normalize each
 *        cell's |psi|^2 against the grid's own 99.9th percentile with a gamma lift into
 *        level[] -- deliberately NOT the 3D cloud's rank curve (orbitalLevelFromRankFraction):
 *        rank-equalizing a uniform grid lights half the screen to ~83% brightness by
 *        construction (see SLICE.md's contrast note); density normalization preserves the
 *        actual falloff (bright lobe cores, near-black tails). Phase sign is NOT stored
 *        (the sequential heatmap ignores it); pc/orbital_slice_pc.py validates signs against
 *        its own computation.
 *
 * @param n/ell/m           Orbital quantum numbers.
 * @param radialCoeff       From laguerreCoeffs(n, ell, ...) -- e.g. OrbitalResampleState::radialCoeff.
 * @param legendreCoeff     From legendreCoeffs(ell, m, ...) -- e.g. OrbitalResampleState::legendreCoeff.
 * @param rRef              This preset's p90 reference radius (OrbitalScale::rRef), in Bohr radii.
 * @param out               [out] Filled in place.
 */
void buildSliceTable(int n, int ell, int m, const orb_real_t *radialCoeff, const orb_real_t *legendreCoeff,
                     orb_real_t rRef, SliceTable *out);

/**
 * @brief Draw one frame of `t` at brightness `fade` (0..1) as a full-resolution heatmap:
 *        one pixel per grid cell (kSliceGridSize == kDisplayWidth, so no block writes and no
 *        interpolation -- each pixel is its own |psi|^2 sample, the imshow-style look of the
 *        reference repo's plots), colored through kSliceColormapStops. Plain overwrite of
 *        every pixel (no blend, no persistence) -- the heatmap is a still image.
 */
void renderSliceFrame(Display &display, const SliceTable &t, orb_real_t fade);
