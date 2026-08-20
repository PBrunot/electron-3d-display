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
 *  - Also auto-advances to a random preset after kViewIdleJumpUs (config/visual_constants.h)
 *    of no tilt input.
 *
 * Everything else is full parity with the original viewer: boot fly-in, breathing zoom,
 * random zoom excursions, point turnover ("buzz"), phase coloring.
 */
#pragma once

#include <cstdint>

#include "render/camera.h"
#include "render/display.h"
#include "physics/orbital_presets.h"
#include "ux/tilt_gesture.h"

/// Fixed (not randomized) point-cloud seed, for a reproducible-looking demo across boots.
inline constexpr uint32_t kOrbitalViewSeed = 12345;

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
    char title[12];
    char orbital_numbers[32];
    orb_real_t baseScale, zoomAmplitude;
    int64_t loadMs = 0; ///< Wall-clock time load() took to build the cloud above, for debug/frame_stats.h's log line.
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
 * @brief Draws one fully-composited frame of `preset` (point cloud, enlarged opaque nucleus
 *        marker, title + quantum-number readout, scale bar) onto `frameBuf` at `scale`/
 *        `camera` -- the exact per-frame content runOrbitalView()'s fly-overs and
 *        steady-state loop draw every frame. Exposed so screenshot_batch.cpp's still-image
 *        capture calls this directly instead of re-implementing a partial copy, keeping
 *        on-device screenshots pixel-identical to what's actually on screen.
 */
void renderOrbitalFrame(uint16_t *frameBuf, const OrbitalPresetState &preset, const CameraState &camera,
                        orb_real_t scale, uint32_t frameSalt, uint32_t buzzThreshold);

/**
 * @brief Run the orbital viewer until a Left tilt-hold confirms.
 * @param display Target display; frames are rendered and presented each loop iteration.
 * @param tilt Gesture source for navigation input.
 */
void runOrbitalView(Display &display, TiltGestureDetector &tilt);
