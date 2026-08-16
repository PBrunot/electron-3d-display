// Multi-electron atom point clouds: combines slater.h's Z_eff/electron-configuration
// model with pointcloud.h's angular/radial-split samplers and angular_library.h's
// compile-time angular tables. Direct (simplified) port of
// micropython/atom_cloud.py's build_atom_point_cloud() -- see that file's module
// docstring for the full model rationale. Simplifications vs the MicroPython version
// for this first device port: no per-point color/shell-brightening and no psi sign
// recomputation (both pure display polish, not physics) -- points carry (n, ell) so a
// caller can still color by shell/subshell itself if wanted.
#pragma once

#include "angular_library.h"
#include "pointcloud.h"
#include "slater.h"

/** One sampled point of a multi-electron atom's point cloud. */
struct AtomPoint {
    orb_real_t x, y, z;
    int n, ell; // which subshell this point came from (e.g. for shell-based coloring)
};

constexpr int kMaxDrawingGroups = 40; // see atom_cloud.h's comment: at most ~1 partial subshell (<=7 groups) + up to ~19 full ones, in Madelung order; generous margin for the hardcoded exceptions too.

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
inline int drawingGroups(const ElectronConfig& config, DrawingGroup* out) {
    int count = 0;
    for (int i = 0; i < config.count; i++) {
        int n = config.subshells[i].n, ell = config.subshells[i].ell, occ = config.subshells[i].occ;
        if (occ == subshellCapacity(ell)) {
            out[count++] = {n, ell, kIsotropicM, occ};
        } else {
            MOcc mOcc[2 * kOrbitalEllMax + 1];
            int mCount = hundFillM(ell, occ, mOcc);
            for (int j = 0; j < mCount; j++) {
                out[count++] = {n, ell, mOcc[j].m, mOcc[j].occ};
            }
        }
    }
    return count;
}

/**
 * Divide `total` points across `count` groups proportional to `weights`, largest-
 * remainder method so counts sum to EXACTLY total instead of drifting from rounding
 * each share independently. Port of atom_cloud._split_counts().
 *
 * @param outCounts  [out] Must hold at least `count` entries.
 */
inline void splitCounts(const int* weights, int count, int total, int* outCounts) {
    int grandTotal = 0;
    for (int i = 0; i < count; i++)
        grandTotal += weights[i];

    orb_real_t shares[kMaxDrawingGroups];
    int assigned = 0;
    for (int i = 0; i < count; i++) {
        shares[i] = orb_real_t(total) * orb_real_t(weights[i]) / orb_real_t(grandTotal);
        outCounts[i] = int(shares[i]);
        assigned += outCounts[i];
    }
    int remainder = total - assigned;

    // Selection sort for the `remainder` largest fractional remainders -- count is at
    // most kMaxDrawingGroups (~40) and remainder < count, so this is cheap enough
    // without needing a real sort.
    bool used[kMaxDrawingGroups] = {};
    for (int r = 0; r < remainder; r++) {
        int best = -1;
        orb_real_t bestFrac = orb_real_t(-1);
        for (int i = 0; i < count; i++) {
            if (used[i])
                continue;
            orb_real_t frac = shares[i] - orb_real_t(outCounts[i]);
            if (frac > bestFrac) {
                bestFrac = frac;
                best = i;
            }
        }
        if (best >= 0) {
            outCounts[best]++;
            used[best] = true;
        }
    }
}

/**
 * Sample `count` points approximating atomic number z's total electron density. Port of
 * atom_cloud.build_atom_point_cloud(), simplified (see this file's header comment).
 *
 * @param z       Atomic number, 1 <= z <= kMaxZ.
 * @param out     [out] Must hold at least `count` entries.
 * @param count   Total points to sample across every occupied subshell.
 * @param seed    RNG seed (same seed -> same points, matching every other port).
 * @return        The electron configuration used (config.count subshells), for
 *                caller-side logging/title display.
 */
inline ElectronConfig buildAtomPointCloud(int z, AtomPoint* out, int count, uint32_t seed) {
    ElectronConfig config = electronConfiguration(z);

    DrawingGroup groups[kMaxDrawingGroups];
    int groupCount = drawingGroups(config, groups);

    int weights[kMaxDrawingGroups];
    for (int i = 0; i < groupCount; i++)
        weights[i] = groups[i].weight;
    int counts[kMaxDrawingGroups];
    splitCounts(weights, groupCount, count, counts);

    XorShift32 rng(seed);
    int idx = 0;
    for (int g = 0; g < groupCount; g++) {
        int n = groups[g].n, ell = groups[g].ell, m = groups[g].m;
        orb_real_t zEff = zEffRadial(z, config, n, ell);
        RadialTable radial = buildRadialSamplerRuntime(n, ell, zEff);

        if (m == kIsotropicM) {
            for (int i = 0; i < counts[g]; i++) {
                OrbitalPoint p = sampleIsotropicPoint(radial, &rng);
                out[idx++] = {p.x, p.y, p.z, n, ell};
            }
        } else {
            const OrbitalAngularTables* angular = findAngularTables(ell, m);
            for (int i = 0; i < counts[g]; i++) {
                OrbitalPoint p = sampleOrientedPoint(radial, *angular, &rng);
                out[idx++] = {p.x, p.y, p.z, n, ell};
            }
        }
    }

    return config;
}
