#include "atom_cloud.h"

#include <algorithm>
#include <cmath>

#include "display.h" // Display::packColor565

int drawingGroups(const ElectronConfig& config, DrawingGroup* out) {
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

void splitCounts(const int* weights, int count, int total, int* outCounts) {
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

ElectronConfig buildAtomPointCloud(int z, AtomPoint* out, int count, uint32_t seed) {
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

const uint8_t* shellBaseRgb(int n) {
    return (n >= 1 && n <= 7) ? kAtomShellRgb[n] : kAtomShellRgb[8];
}

OuterSubshell outerSubshellRRef(const AtomPoint* points, int count, const ElectronConfig& config) {
    // Static, not stack-local: see orbital_presets.cpp's computeOrbitalLevels() comment --
    // this project avoids large stack arrays after a real task stack overflow.
    static orb_real_t radii[kAtomMaxPoints];

    OuterSubshell best;
    orb_real_t bestR = orb_real_t(-1);
    for (int s = 0; s < config.count; s++) {
        int n = config.subshells[s].n, ell = config.subshells[s].ell;
        int matchCount = 0;
        for (int i = 0; i < count; i++) {
            if (points[i].n == n && points[i].ell == ell) {
                orb_real_t x = points[i].x, y = points[i].y, z = points[i].z;
                radii[matchCount++] = std::sqrt(x * x + y * y + z * z);
            }
        }
        if (matchCount == 0)
            continue;
        std::sort(radii, radii + matchCount);
        int idx = int(orb_real_t(0.90) * orb_real_t(matchCount - 1));
        if (idx >= matchCount)
            idx = matchCount - 1;
        orb_real_t rRef = radii[idx] > orb_real_t(1e-6) ? radii[idx] : orb_real_t(1);
        if (rRef > bestR) {
            bestR = rRef;
            best = {n, ell, rRef};
        }
    }
    return best;
}

static uint8_t brightenChannel(uint8_t c, orb_real_t factor) {
    return uint8_t(orb_real_t(c) + (orb_real_t(255) - orb_real_t(c)) * factor);
}

static uint8_t dimChannel(uint8_t c, orb_real_t factor) {
    return uint8_t(orb_real_t(c) * (orb_real_t(1) - factor));
}

void colorizeAtomPoints(const AtomPoint* points, int count, const OuterSubshell& outer, uint16_t* outColors) {
    for (int i = 0; i < count; i++) {
        const uint8_t* base = shellBaseRgb(points[i].n);
        bool isOuter = points[i].n == outer.n && points[i].ell == outer.ell;
        uint8_t r, g, b;
        if (isOuter) {
            r = brightenChannel(base[0], kAtomOuterShellBrighten);
            g = brightenChannel(base[1], kAtomOuterShellBrighten);
            b = brightenChannel(base[2], kAtomOuterShellBrighten);
        } else {
            r = dimChannel(base[0], kAtomInnerShellDim);
            g = dimChannel(base[1], kAtomInnerShellDim);
            b = dimChannel(base[2], kAtomInnerShellDim);
        }
        outColors[i] = Display::packColor565(r, g, b);
    }
}

orb_real_t pixelsPerBohrForCanvas(orb_real_t canvasCenter, orb_real_t radiusFraction, int referenceZ) {
    static AtomPoint calibPoints[kAtomCalibrationPoints];
    ElectronConfig config = buildAtomPointCloud(referenceZ, calibPoints, kAtomCalibrationPoints, kAtomCloudSeed);
    OuterSubshell outer = outerSubshellRRef(calibPoints, kAtomCalibrationPoints, config);
    orb_real_t targetPx = radiusFraction * canvasCenter;
    return targetPx / outer.rRef;
}

AtomScale scaleForAtom(orb_real_t rRef, orb_real_t pixelsPerBohr, orb_real_t amplitudeFraction) {
    return AtomScale{pixelsPerBohr, pixelsPerBohr * amplitudeFraction, rRef};
}
