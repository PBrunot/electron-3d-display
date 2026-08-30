/**
 * @file atom_view.h
 * @brief Multi-electron atom point-cloud viewer.
 *
 * Shell-colored by principal quantum number n (valence subshell brightened, core dimmed),
 * per-element-renormalized scale (every element's outer subshell reaches the same on-screen
 * radius -- see atom_cloud.h's scaleForAtom()/kAtomTargetPx), per-shell-colored wrapped
 * electron-configuration title. Built on atom_cloud.h's model layer and camera.h's
 * render/fly-over pipeline; see orbital_view.h for the sibling hydrogen-orbital viewer this
 * mirrors. Unlike orbital_view.h, there's no point-turnover: atom_cloud's cloud is a mixture
 * of several subshells built once and stays static.
 *
 * Tilt-gesture controls (see tilt_gesture.h):
 *  - Up/Down tilt-hold: step through every element in periodic-table order
 *    (periodic_grid.h's periodicTableSnakeStep()), Down = forward, Up = backward.
 *  - Left tilt-hold: return to chooser.h's menu (runAtomView() returns).
 *  - Right tilt-hold: run the on-device shell dissection sequence
 *    (runDissectionSequence(), file-local to atom_view.cpp, built on atom_cloud.h's
 *    subshellDissectionPlan()) -- one hold automatically peels through every occupied
 *    subshell outer to inner, easing the camera in to frame each newly-revealed outermost
 *    remaining shell and holding briefly before moving to the next, then easing back to the
 *    full atom (holding there briefly before control returns). A second Right tilt-hold
 *    during the sequence cancels it early, easing straight back to the full atom the same
 *    way. Simplified relative to pc/atom_view_pc.py's PC dissection: whole shells are
 *    dropped as they're peeled away (rather than a camera-space half-clip cutaway -- no
 *    per-frame clip-plane test needed, and arguably clearer on a 240x240 panel), and there's
 *    no phase/sign coloring.
 *  - Also auto-advances to a random element after kViewIdleJumpUs (config/visual_constants.h)
 *    of no tilt input -- on CYD (no IMU, see main.cpp's CYD boot branch), that same idle
 *    timeout instead has a kViewCrossSwitchProbability chance to return to main.cpp's loop
 *    for the orbital viewer, since CYD can never reach chooser.cpp's own idle coin-flip.
 */
#pragma once

#include <cstdint>

#include "physics/atom_cloud.h"
#include "render/camera.h"
#include "render/display.h"
#include "render/font.h"
#include "ux/tilt_gesture.h"

/// Default element shown on first boot (carbon). Every element renormalizes to the same
/// on-screen radius (see atom_cloud.h's kAtomTargetPx), so there's no size-based reason to
/// prefer one default over another.
inline constexpr int kAtomViewDefaultZ = 6;

/**
 * @brief Everything one loaded element needs to render.
 *
 * Point coordinates, plus per-SUBSHELL (not per-point, see atom_cloud.h's AtomSubshellRange)
 * point ranges and their shell-colored (brightened/dimmed) render groups, and the electron
 * configuration used (for the title).
 *
 * `points` is a non-owning pointer, not an embedded array: runAtomView()'s own `preset`
 * instance binds it to physics/view_steady_arena.h's shared ViewSteadyArena (once, on first
 * use); other instances (debug/screenshot_batch.cpp's/debug/gif_capture_test.cpp's own
 * capture-only presets) bind it to their own private backing storage instead -- see that
 * header's comment for why the two must never share. Every existing `preset.points[i]` call
 * site keeps working unchanged either way (pointer indexing reads identically to array
 * indexing).
 */
struct AtomPresetState
{
    AtomPoint *points = nullptr;
    AtomSubshellRange ranges[kMaxConfigSubshells]; ///< This element's subshells and their point ranges.
    int rangeCount = 0;
    PointGroup groups[kMaxConfigSubshells]; ///< `ranges` paired with each subshell's render color.
    int groupCount = 0;
    ElectronConfig config;
    int z = 0;
    orb_real_t baseScale, zoomAmplitude, rRef; ///< rRef: outer subshell's own reference radius,
                                                ///< for drawBoundingCircle() (render/overlay.h).
    int64_t loadMs = 0; ///< Wall-clock time load() took to build the cloud above, for debug/frame_stats.h's log line.

    /// Build this element's point cloud, subshell ranges/colors, and renormalized scale.
    void load(int zIn);
};

/**
 * @brief Draw the element's symbol at (x, y) in kFontHuge -- its own true pixel size, not
 *        an upscaled smaller font.
 */
void drawAtomTitle(Display &display, int x, int y, int z, uint16_t textColor);

/**
 * @brief Draws one fully-composited frame of `preset` (point cloud, opaque nucleus marker,
 *        bounding circle, title + Z-number label, scale bar) onto `display` at `scale`/
 *        `camera` -- the exact per-frame content runAtomView()'s fly-overs and steady-state
 *        loop draw every frame (outside the shell-dissection sequence, which has its own
 *        title). Exposed so screenshot_batch.cpp's still-image capture calls this directly
 *        instead of re-implementing a partial copy, keeping on-device screenshots
 *        pixel-identical to what's actually on screen.
 */
void renderAtomFrame(Display &display, const AtomPresetState &preset, const CameraState &camera, orb_real_t scale,
                     uint32_t frameSalt, uint32_t buzzThreshold);

/**
 * @brief Renders one still frame of `preset`'s shell-dissection view at dissection `level`
 *        (1-based: 1 = full atom with its outermost shell highlighted, up to the returned
 *        count = only the innermost shell left) onto `display` -- the same peeled-shell
 *        view, re-normalized scale, and shell-notation title runDissectionSequence() (in
 *        atom_view.cpp) holds on mid-sequence. Computes its own dissection plan from
 *        `preset` (via physics/atom_cloud.h's subshellDissectionPlan()) rather than reading
 *        any state left over from a live view's dissection, so it's safe to call with an
 *        unrelated preset (e.g. screenshot_batch.cpp's own instance) at any time.
 * @param level  Out-of-range values (< 1 or > the returned count) draw nothing.
 * @return       The number of dissectable shells for `preset`, i.e. the valid level range.
 */
int renderAtomDissectFrame(Display &display, const AtomPresetState &preset, const CameraState &camera, int level);

/**
 * @brief Run the atom viewer until a Left tilt-hold confirms.
 * @param display Target display; frames are rendered and presented each loop iteration.
 * @param tilt Gesture source for navigation input.
 */
void runAtomView(Display &display, TiltGestureDetector &tilt);
