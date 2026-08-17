// Hydrogen orbital point-cloud viewer: run loop + preset-state, built on
// orbital_presets.h's model layer and camera.h's render/fly-over pipeline. Port of
// micropython/orbital_view.py.
//
// Tilt-gesture-driven preset cycling (see tilt_gesture.h): Right/Left tilt-hold advances/
// goes back a preset in orbital_library.h (eased via camera.h's kSwitchStartScaleFactor/
// kSwitchTransitionFrames, same constants the boot intro's fly-in already uses at a bigger
// scale factor); Up tilt-hold returns to chooser.h's menu (this function then returns);
// Down tilt-hold is logged but otherwise a no-op here -- it means "dissect" in atom_view.h,
// and hydrogen orbitals have no subshell structure to dissect. Everything else is full
// parity with the MicroPython port: boot fly-in, breathing zoom, random zoom excursions,
// point turnover ("buzz"), phase coloring.
#pragma once

#include <cstdint>

#include "camera.h"
#include "display.h"
#include "orbital_presets.h"
#include "tilt_gesture.h"

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
 * Run the orbital viewer until an Up tilt-hold confirms (see this file's header comment),
 * at which point this returns so the caller (chooser.h) can show the menu again.
 */
void runOrbitalView(Display &display, TiltGestureDetector &tilt);
