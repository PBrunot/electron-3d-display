// Multi-electron atom point clouds: combines slater.h's Z_eff/electron-configuration
// model with pointcloud.h's angular/radial-split samplers and angular_library.h's
// compile-time angular tables. Port of micropython/atom_cloud.py's
// build_atom_point_cloud() -- see that file's module docstring for the full model
// rationale, and its "Coloring" paragraph for the shell-color/brighten-dim rationale
// specifically. Points carry (n, ell) so a caller can color by shell/subshell itself --
// see colorizeAtomPoints() below (M4). Simplification vs the MicroPython version: no psi
// sign recomputation for anisotropic (Hund's-rule) groups -- that only feeds
// pc/atom_view_pc.py's PC-only shell-dissection view's phase coloring (one exploded shell
// at a time). This project DOES now have an on-device dissection view (atom_view.cpp,
// Down-tilt gesture, built on subshellDissectionPlan() below), but a deliberately simpler
// one -- shell color only, no phase/sign -- so nothing here consumes per-point sign either.
#pragma once

#include <cstdint>

#include "angular_library.h"
#include "pointcloud.h"
#include "slater.h"

/** One sampled point of a multi-electron atom's point cloud. */
struct AtomPoint {
    orb_real_t x, y, z;
    int n, ell; // which subshell this point came from (e.g. for shell-based coloring)
};

constexpr int kMaxDrawingGroups = 40; // see atom_cloud.h's comment: at most ~1 partial subshell (<=7 groups) + up to ~19 full ones, in Madelung order; generous margin for the hardcoded exceptions too.
constexpr int kAtomMaxPoints = 4096; // static scratch bound for outerSubshellRRef()'s per-subshell radius sort

struct DrawingGroup {
    int n, ell, m; // m == kIsotropicM means "full subshell, sample isotropically"
    int weight;    // electron count this group represents
};
constexpr int kIsotropicM = -999;

/**
 * Expand `config` into per-drawing groups. A FULL subshell (occ == capacity) becomes
 * ONE isotropic group (m = kIsotropicM) -- exact by Unsoeld's theorem and cheaper than
 * building 2*ell+1 separate angular groups for a subshell whose total density has no
 * anisotropy anyway. A non-full subshell expands into one group per individually
 * occupied real orbital (Hund's rule, slater.hundFillM()). Port of
 * atom_cloud._drawing_groups().
 *
 * @return  Number of groups written to out (must hold at least kMaxDrawingGroups).
 */
int drawingGroups(const ElectronConfig& config, DrawingGroup* out);

/**
 * Divide `total` points across `count` groups proportional to `weights`, largest-
 * remainder method so counts sum to EXACTLY total instead of drifting from rounding
 * each share independently. Port of atom_cloud._split_counts().
 *
 * @param outCounts  [out] Must hold at least `count` entries.
 */
void splitCounts(const int* weights, int count, int total, int* outCounts);

/**
 * Sample `count` points approximating atomic number z's total electron density. Port of
 * atom_cloud.build_atom_point_cloud() (coloring split out separately, see
 * colorizeAtomPoints() below).
 *
 * @param z       Atomic number, 1 <= z <= kMaxZ.
 * @param out     [out] Must hold at least `count` entries.
 * @param count   Total points to sample across every occupied subshell.
 * @param seed    RNG seed (same seed -> same points, matching every other port).
 * @return        The electron configuration used (config.count subshells), for
 *                caller-side logging/title display.
 */
ElectronConfig buildAtomPointCloud(int z, AtomPoint* out, int count, uint32_t seed);

// --- Shell coloring (M4) ---
//
// Every point's default color is by shell (principal quantum number n), so the classic
// K/L/M/N shell structure reads visually across the whole atom -- see kAtomShellRgb.
// Points belonging to the OUTERMOST subshell (see outerSubshellRRef(), the same subshell
// the scale calibration below treats as "the atom's size") are brightened toward white;
// every other point is dimmed toward black, widening the contrast further -- without this
// the valence subshell (a small minority of the total point budget for any atom past neon)
// is easy to miss entirely. Port of atom_cloud.py's SHELL_RGB / _brighten_outer_shell() /
// _dim_inner_shell() and their application loop in build_atom_point_cloud() -- see that
// file's "Coloring" docstring paragraph for the full rationale.
//
// Index 0 unused (n starts at 1); index 8 is the fallback for n>7, unreachable for any
// z<=kMaxZ ground-state configuration but kept so an out-of-range n degrades to a color
// instead of an out-of-bounds read.
constexpr uint8_t kAtomShellRgb[9][3] = {
    {255, 255, 255}, // unused (n=0)
    {255, 80, 80},   // n=1 K
    {255, 170, 60},  // n=2 L
    {255, 230, 60},  // n=3 M
    {120, 230, 90},  // n=4 N
    {70, 200, 220},  // n=5 O
    {110, 130, 255}, // n=6 P
    {200, 100, 240}, // n=7 Q
    {160, 160, 160}, // fallback, n>7
};

/** kAtomShellRgb[n], clamped to the fallback entry for n outside [1, 7]. */
const uint8_t* shellBaseRgb(int n);

// atom_cloud.py's own OUTER_SHELL_BRIGHTEN=0.92 pushes almost to pure white, tuned for a
// renderer that either draws the point outright with no discount (its "device path"
// comment) or discounts it via PC's own ELECTRON_ALPHA blend -- either way, a single hit
// already reads as near-white there. Kept lower here (0.6, closer to that same docstring's
// other quoted figure of 0.55, "read as only a modest change") since this renderer ALSO
// alpha-blends (camera.h's kElectronAlphaQ8) and the valence subshell typically covers a
// much larger on-screen area than the dimmed core -- 0.92 read as "the atom is just white"
// rather than "the valence shell stands out from a colored core" once actually seen on
// this panel.
constexpr orb_real_t kAtomOuterShellBrighten = orb_real_t(0.6); // lerp toward white
constexpr orb_real_t kAtomInnerShellDim = orb_real_t(0.2);      // scale toward black

/**
 * Which subshell (n, ell) has the largest measured p90 radius in THIS specific point
 * cloud, and that radius -- this is what actually defines an atom's on-screen size and
 * which points get brightened by colorizeAtomPoints(), NOT the whole cloud's own p90
 * (dominated by core electrons for any atom past helium, since points are split strictly
 * proportional to electron count). Port of atom_cloud.outer_subshell_r_ref(), simplified:
 * only the single outermost entry of subshellDissectionPlan() below is needed for scale
 * calibration, so this returns just that one entry instead of building/sorting the full
 * plan -- callers that DO need the full sorted list (atom_view.cpp's dissection view) call
 * subshellDissectionPlan() directly instead.
 */
struct OuterSubshell {
    int n = 0, ell = 0;
    orb_real_t rRef = orb_real_t(1);
};
OuterSubshell outerSubshellRRef(const AtomPoint* points, int count, const ElectronConfig& config);

/** One subshell's identity, p90 radius, and electron occupancy, as produced by
 * subshellDissectionPlan() below. */
struct DissectionEntry {
    int n, ell;
    orb_real_t rRef;
    int occ; // this subshell's own electron count, e.g. 4 for 2p4 -- see ElectronConfig::occ
};

/**
 * Every occupied subshell's (n, ell) and p90 radius (same measure outerSubshellRRef() uses),
 * sorted descending by radius -- outermost first, innermost last. Port of
 * atom_cloud.subshell_dissection_plan(), device-path subset: p90 radius only, no phase/sign
 * recomputation (this project's on-device dissection has no phase coloring, same "not worth
 * the effort" call this file's header comment already makes about that). Used by
 * atom_view.cpp's on-device shell-by-shell dissection (Down-tilt gesture).
 *
 * @param out  [out] Must hold at least kMaxConfigSubshells entries.
 * @return     Number of entries written (== config.count, minus any subshell with zero
 *             matched points in this specific cloud).
 */
int subshellDissectionPlan(const AtomPoint* points, int count, const ElectronConfig& config, DissectionEntry* out);

/**
 * Pack point i's shell color (shellBaseRgb(points[i].n)) into outColors[i], brightened
 * toward white if it belongs to `outer`'s subshell (kAtomOuterShellBrighten) or dimmed
 * toward black otherwise (kAtomInnerShellDim).
 */
void colorizeAtomPoints(const AtomPoint* points, int count, const OuterSubshell& outer, uint16_t* outColors);

constexpr uint32_t kAtomCloudSeed = 12345; // matches micropython/atom_cloud.py's SEED

// --- Scale (M4, revised) ---
//
// Deviation from atom_cloud.py: that model deliberately uses ONE fixed pixels-per-Bohr-
// radius, calibrated against Rubidium (Z=37), so switching elements shows the real
// periodic size trend (noble gases small and tight, alkali metals big and diffuse).
// Abandoned here per explicit user decision after seeing it on this panel: a 240px screen
// with no zoom control yet (that lands in M5) makes most elements read as "just far away"
// under that scheme. Every element now renormalizes to the SAME on-screen target radius
// instead, exactly like orbital_presets.h's scaleFromRadii() already does for hydrogen
// presets -- consistent, always-legible size at the cost of the periodic size trend.
constexpr orb_real_t kAtomTargetPx = orb_real_t(75); // p90 outer-subshell radius, in pixels, at rest

constexpr orb_real_t kAtomZoomAmplitudeFraction = orb_real_t(0.4); // matches cloud_common.ZOOM_AMPLITUDE_FRACTION

struct AtomScale {
    orb_real_t baseScale, zoomAmplitude, rRef;
};

/**
 * Like orbital_presets.h's scaleFromRadii(): baseScale = kAtomTargetPx / rRef, so every
 * element's outer subshell reaches the same on-screen radius regardless of its actual
 * physical size. `rRef` must be outerSubshellRRef()'s result, not the whole cloud's p90 --
 * see that function's docstring.
 */
AtomScale scaleForAtom(orb_real_t rRef, orb_real_t amplitudeFraction = kAtomZoomAmplitudeFraction);
