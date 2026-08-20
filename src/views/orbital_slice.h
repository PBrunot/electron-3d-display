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

#include "physics/orbitals.h"        // orb_real_t
#include "config/visual_constants.h" // kSliceGridSize

/**
 * @brief One built slice: the (n, ell, m) it was built for, the static lobe-plane azimuth and
 *        grid framing, and the per-cell brightness/sign the heatmap is drawn from every frame.
 *
 * The plane is fixed at build time (D2, SLICE.md section 3/4) -- there is no per-frame trig,
 * so every frame of a sequence just re-reads level[]/sign[] at a different fade.
 */
struct SliceTable
{
    int n, ell, m;
    orb_real_t planeAzimuth; ///< Lobe-plane ph0: 0 for m >= 0, pi/(2|m|) for m < 0.
    orb_real_t extentBohr;   ///< Grid half-extent, kSliceFramingFactor * rRef, in Bohr radii.
    orb_real_t extentPm;     ///< Same, in picometers -- feeds drawScaleBar().
    uint8_t level[kSliceGridSize * kSliceGridSize]; ///< Density-normalized brightness: 255*min(1,|psi|^2/v99)^kSliceLevelGamma (see buildSliceTable()).
    int8_t sign[kSliceGridSize * kSliceGridSize];   ///< Sign of the cell value (phase).
};

/**
 * @brief One-time build: sample base = R(r)*P(theta) at every cell center, fold in the
 *        per-half azimuthal sign flip (see SLICE.md section 3), then density-normalize each
 *        cell's |psi|^2 against the grid's own 99.9th percentile with a gamma lift into
 *        level[] -- deliberately NOT the 3D cloud's rank curve (orbitalLevelFromRankFraction):
 *        rank-equalizing a uniform grid lights half the screen to ~83% brightness by
 *        construction (see SLICE.md's contrast note); density normalization preserves the
 *        actual falloff (bright lobe cores, near-black tails).
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
 * @brief Draw one frame of `t` at brightness `fade` (0..1), as kSliceCellPx x kSliceCellPx
 *        flat-colored blocks -- a plain overwrite of every pixel (no blend, no persistence),
 *        since the heatmap is a still image, not a moving point cloud.
 */
void renderSliceFrame(uint16_t *frameBuf, const SliceTable &t, orb_real_t fade);
