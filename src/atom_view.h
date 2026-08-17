// Multi-electron atom point-cloud viewer: shell-colored (K/L/M/... by principal quantum
// number n, valence subshell brightened / core dimmed), per-element-renormalized scale
// (every element's outer subshell reaches the same on-screen radius, see atom_cloud.h's
// scaleForAtom()/kAtomTargetPx), per-shell-colored wrapped electron-configuration title.
// Port of micropython/atom_view.py, built on atom_cloud.h's model layer and camera.h's
// render/fly-over pipeline -- see orbital_view.h for the sibling hydrogen-orbital viewer
// this mirrors.
//
// Not the default boot animation (CLAUDE.md's M1-M4 roadmap is hydrogen orbitals first) --
// an alternate entry point like atom_view_test.h's M1/M2 placeholder, but this is the real
// parity build. No point-turnover here (unlike orbital_view.h) -- atom_cloud's cloud is a
// mixture of several subshells built once and stays static, matching
// atom_cloud.py's module docstring on that tradeoff.
//
// Tilt-gesture-driven controls (see tilt_gesture.h): Right/Left tilt-hold steps the
// element Z up/down (wrapping 1..kMaxZ); Up tilt-hold returns to chooser.h's menu (this
// function then returns); Down tilt-hold triggers the on-device shell dissection sequence
// (runDissectionSequence(), file-local to atom_view.cpp, built on atom_cloud.h's
// subshellDissectionPlan()) -- a simplified device-path port of pc/atom_view_pc.py's
// shell-dissection sequence (D key): ONE Down-hold automatically peels through every
// occupied subshell outer to inner (no further gesture needed mid-sequence, matching the
// PC version's own one-shot blocking sequence), easing the camera in to frame each newly-
// revealed outermost remaining shell and holding briefly before moving to the next, then
// easing back to the full atom at the end. Two deliberate simplifications vs the PC
// version: whole shells are DROPPED as they're peeled away (rather than a camera-space
// half-clip cutaway -- no per-frame clip-plane test needed, and arguably clearer on a
// 240x240 panel: a whole shell vanishes instead of being sliced in half), and there's no
// phase/sign coloring (matches atom_cloud.h's existing on-device simplification).
#pragma once

#include <cstdint>

#include "atom_cloud.h"
#include "camera.h"
#include "display.h"
#include "font.h"
#include "tilt_gesture.h"

constexpr int kAtomViewNumPoints = 3000; // matches atom_view.py's N_POINTS (device budget)
static_assert(kAtomViewNumPoints <= kAtomMaxPoints,
              "kAtomViewNumPoints exceeds atom_cloud.h's static scratch bound");
constexpr int kAtomViewDefaultZ = 6; // carbon, matches atom_view.py's DEFAULT_Z -- every
                                      // element now renormalizes to the same on-screen
                                      // radius (see atom_cloud.h's kAtomTargetPx), so
                                      // there's no size-based reason left to deviate

/**
 * Everything one loaded element needs to render: point coordinates, their shell-colored
 * (brightened/dimmed) encoded colors, and the electron configuration used (for the
 * title). Port of atom_view.py's AtomPresetState.
 */
struct AtomPresetState {
    AtomPoint points[kAtomViewNumPoints];
    uint16_t colors[kAtomViewNumPoints];
    ElectronConfig config;
    int z = 0;
    orb_real_t baseScale, zoomAmplitude;

    /** Build this element's point cloud, shell colors, and renormalized scale. */
    void load(int zIn);
};

/**
 * Draw the element's symbol big (see atom_view.cpp's kAtomTitleSymbolScale), "Z=<z>"
 * underneath at plain size -- same big-hero/small-caption shape as
 * atom_view.cpp's drawDissectTitle() and scrollElementIntro(), for one consistent look
 * across the app. Feedback (2026-08-17): dropped the earlier per-subshell electron-
 * configuration breakdown ("1s2 2s2 2p2 ...") this used to also draw -- too much detail
 * for the main screen; still available via the title-callback lambda diff if a future
 * screen wants it back.
 */
void drawAtomTitle(uint16_t* frameBuf, int x, int y, int z, uint16_t textColor, const Font& font);

/**
 * Run the atom viewer until an Up tilt-hold confirms (see this file's header comment), at
 * which point this returns so the caller (chooser.h) can show the menu again.
 */
void runAtomView(Display& display, TiltGestureDetector& tilt);
