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
// atom_cloud.py's module docstring on that tradeoff. Nudge-driven Z switching lands in M5
// -- for now runAtomView() always shows kAtomViewDefaultZ (carbon) and never returns (the
// menu it would return to doesn't exist yet either, that's M6).
#pragma once

#include <cstdint>

#include "atom_cloud.h"
#include "camera.h"
#include "display.h"

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
 * Draw "<Symbol> (Z=<z>) " in `textColor`, then each subshell of `config` (e.g. "2p2")
 * colored by its own shell (shellBaseRgb(n)) -- so the on-screen label and the rendered
 * cloud read as one color language. Wraps to a new line instead of running off the right
 * edge. Port of atom_view.py's _draw_atom_title().
 */
void drawAtomTitle(uint16_t* frameBuf, int x, int y, int z, const ElectronConfig& config, uint16_t textColor);

/**
 * Run the atom viewer, forever (see this file's header comment on why it never returns
 * yet).
 */
void runAtomView(Display& display);
