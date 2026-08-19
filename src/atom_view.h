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
 *    full atom. Simplified relative to pc/atom_view_pc.py's PC dissection: whole shells are
 *    dropped as they're peeled away (rather than a camera-space half-clip cutaway -- no
 *    per-frame clip-plane test needed, and arguably clearer on a 240x240 panel), and there's
 *    no phase/sign coloring.
 *  - Also auto-advances to a random element after kIdleJumpUs (atom_view.cpp) of no tilt
 *    input.
 */
#pragma once

#include <cstdint>

#include "atom_cloud.h"
#include "camera.h"
#include "display.h"
#include "font.h"
#include "tilt_gesture.h"

/// Default element shown on first boot (carbon). Every element renormalizes to the same
/// on-screen radius (see atom_cloud.h's kAtomTargetPx), so there's no size-based reason to
/// prefer one default over another.
constexpr int kAtomViewDefaultZ = 6;

/**
 * @brief Everything one loaded element needs to render.
 *
 * Point coordinates, plus per-SUBSHELL (not per-point, see atom_cloud.h's AtomSubshellRange)
 * point ranges and their shell-colored (brightened/dimmed) render groups, and the electron
 * configuration used (for the title).
 */
struct AtomPresetState
{
    AtomPoint points[kAtomNumPoints];
    AtomSubshellRange ranges[kMaxConfigSubshells]; ///< This element's subshells and their point ranges.
    int rangeCount = 0;
    PointGroup groups[kMaxConfigSubshells]; ///< `ranges` paired with each subshell's render color.
    int groupCount = 0;
    ElectronConfig config;
    int z = 0;
    orb_real_t baseScale, zoomAmplitude;

    /// Build this element's point cloud, subshell ranges/colors, and renormalized scale.
    void load(int zIn);
};

/**
 * @brief Draw the element's symbol big with "Z=<z>" underneath at plain size.
 *
 * Same big-hero/small-caption shape as atom_view.cpp's drawDissectTitle() and
 * scrollElementIntro(), for one consistent look across the app.
 */
void drawAtomTitle(uint16_t *frameBuf, int x, int y, int z, uint16_t textColor, const Font &font);

/**
 * @brief Run the atom viewer until a Left tilt-hold confirms.
 * @param display Target display; frames are rendered and presented each loop iteration.
 * @param tilt Gesture source for navigation input.
 */
void runAtomView(Display &display, TiltGestureDetector &tilt);
