// Hydrogen orbital point-cloud viewer: run loop + preset-state, built on
// orbital_presets.h's model layer and camera.h's render/fly-over pipeline. Port of
// micropython/orbital_view.py.
//
// Nudge-driven preset cycling lands in M5 -- for now runOrbitalView() always shows
// orbital_library.h's default preset (2pz) and never returns (the menu it would return to
// doesn't exist yet either, that's M6). Everything else is full parity: boot fly-in,
// breathing zoom, random zoom excursions, point turnover ("buzz"), phase coloring.
#pragma once

#include <cstdint>

#include "camera.h"
#include "display.h"
#include "orbital_presets.h"

constexpr int kOrbitalViewNumPoints = 3000; // matches cloud_common.N_POINTS (device budget)
static_assert(kOrbitalViewNumPoints <= kOrbitalMaxPoints,
              "kOrbitalViewNumPoints exceeds orbital_presets.h's static scratch bound");
constexpr uint32_t kOrbitalViewSeed = 12345; // fixed for a reproducible-looking demo across boots

/**
 * Everything one loaded preset needs to render and turn over: point coordinates (kept
 * alive for resamplePoints()), their encoded colors, and the OrbitalResampleState to keep
 * resampling from the same distribution later. Port of orbital_view.py's PresetState.
 */
struct OrbitalPresetState
{
    OrbitalPoint points[kOrbitalViewNumPoints];
    uint16_t colors[kOrbitalViewNumPoints];
    OrbitalResampleState resample;
    char title[32];
    orb_real_t baseScale, zoomAmplitude;

    /** Build this preset's point cloud, colors, and scale from orbital_library.h[index]. */
    void load(int index);

    /** Point turnover (see kOrbitalCullFraction/kOrbitalCullRefreshFrames): redraw `count`
     * points from the same distribution, in place. */
    void resamplePoints(int count);
};

/**
 * Run the orbital viewer, forever (see this file's header comment on why it never
 * returns yet).
 */
void runOrbitalView(Display &display);
