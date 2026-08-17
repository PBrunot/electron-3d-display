// Hydrogen orbital point-cloud viewer: run loop + preset-state, built on
// orbital_presets.h's model layer and camera.h's render/fly-over pipeline. Port of
// micropython/orbital_view.py.
//
// Tilt-gesture-driven preset cycling (see tilt_gesture.h): Down/Up tilt-hold advances/goes
// back a preset in orbital_library.h (eased via camera.h's kSwitchStartScaleFactor/
// kSwitchTransitionFrames, same constants the boot intro's fly-in already uses at a bigger
// scale factor, plus the new quantum-number reveal, see scrollOrbitalIntro() in
// orbital_view.cpp); Left tilt-hold returns to chooser.h's menu (this function then
// returns); Right tilt-hold is logged but otherwise a no-op here -- it means "dissect" in
// atom_view.h, and hydrogen orbitals have no subshell structure to dissect. Also
// auto-advances to a random preset after kIdleJumpUs of no tilt input. Everything else is
// full parity with the MicroPython port: boot fly-in, breathing zoom, random zoom
// excursions, point turnover ("buzz"), phase coloring.
#pragma once

#include <cstdint>

#include "camera.h"
#include "display.h"
#include "orbital_presets.h"
#include "tilt_gesture.h"

static constexpr uint32_t kOrbitalViewSeed = 12345; // fixed for a reproducible-looking demo across boots

/**
 * Everything one loaded preset needs to render and turn over: point coordinates (kept
 * alive for resamplePoints()), their encoded colors, and the OrbitalResampleState to keep
 * resampling from the same distribution later. Port of orbital_view.py's PresetState.
 */
struct OrbitalPresetState
{
    OrbitalPoint points[kOrbitalNumPoints];
    uint16_t colors[kOrbitalNumPoints];
    OrbitalResampleState resample;
    char title[32];
    orb_real_t baseScale, zoomAmplitude;
    // This preset's phase-color pair (see orbital_library.h's OrbitalDescriptor) --
    // kept here so resamplePoints() re-encodes turned-over points in the same colors.
    uint8_t posRgb[3];
    uint8_t negRgb[3];

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
