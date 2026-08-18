/**
 * @file orbital_view.h
 * @brief Hydrogen orbital point-cloud viewer: run loop + preset state.
 *
 * Built on orbital_presets.h's model layer and camera.h's render/fly-over pipeline.
 *
 * Tilt-gesture controls (see tilt_gesture.h):
 *  - Down/Up tilt-hold: advance/go back a preset in orbital_library.h, eased via
 *    camera.h's kSwitchStartScaleFactor/kSwitchTransitionFrames plus a quantum-number
 *    reveal (scrollOrbitalIntro() in orbital_view.cpp).
 *  - Left tilt-hold: return to chooser.h's menu (runOrbitalView() returns).
 *  - Right tilt-hold: logged but otherwise a no-op -- it means "dissect" in atom_view.h,
 *    and hydrogen orbitals have no subshell structure to dissect.
 *  - Also auto-advances to a random preset after kIdleJumpUs (orbital_view.cpp) of no tilt
 *    input.
 *
 * Everything else is full parity with the original viewer: boot fly-in, breathing zoom,
 * random zoom excursions, point turnover ("buzz"), phase coloring.
 */
#pragma once

#include <cstdint>

#include "camera.h"
#include "display.h"
#include "orbital_presets.h"
#include "tilt_gesture.h"

/// Fixed (not randomized) point-cloud seed, for a reproducible-looking demo across boots.
static constexpr uint32_t kOrbitalViewSeed = 12345;

/**
 * @brief Everything one loaded preset needs to render and turn over.
 *
 * Point coordinates (kept alive for resamplePoints()), their encoded colors, and the
 * OrbitalResampleState needed to keep resampling from the same distribution later.
 */
struct OrbitalPresetState
{
    OrbitalPoint points[kOrbitalNumPoints];
    uint16_t colors[kOrbitalNumPoints];
    OrbitalResampleState resample;
    char title[32];
    orb_real_t baseScale, zoomAmplitude;
    /// This preset's phase-color pair (see orbital_library.h's OrbitalDescriptor), kept here
    /// so resamplePoints() re-encodes turned-over points in the same colors.
    uint8_t posRgb[3];
    uint8_t negRgb[3];

    /// Build this preset's point cloud, colors, and scale from orbital_library.h[index].
    void load(int index);

    /// Point turnover (see kOrbitalCullFraction/kOrbitalCullRefreshFrames): redraw `count`
    /// points from the same distribution, in place.
    void resamplePoints(int count);
};

/**
 * @brief Run the orbital viewer until a Left tilt-hold confirms.
 * @param display Target display; frames are rendered and presented each loop iteration.
 * @param tilt Gesture source for navigation input.
 */
void runOrbitalView(Display &display, TiltGestureDetector &tilt);
