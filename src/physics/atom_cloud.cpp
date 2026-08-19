#include "physics/atom_cloud.h"
#include "physics/atom_size_calib.h"
#include "physics/hfs_radial.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "render/display.h"  // Display::packColor565
#include "esp_attr.h" // EXT_RAM_BSS_ATTR

int drawingGroups(const ElectronConfig &config, DrawingGroup *out)
{
    int count = 0;
    for (int i = 0; i < config.count; i++)
    {
        int n = config.subshells[i].n, ell = config.subshells[i].ell, occ = config.subshells[i].occ;
        if (occ == subshellCapacity(ell))
        {
            out[count++] = {n, ell, kIsotropicM, occ, i};
        }
        else
        {
            MOcc mOcc[2 * kOrbitalEllMax + 1];
            int mCount = hundFillM(ell, occ, mOcc);
            for (int j = 0; j < mCount; j++)
            {
                out[count++] = {n, ell, mOcc[j].m, mOcc[j].occ, i};
            }
        }
    }
    return count;
}

void splitCounts(const int *weights, int count, int total, int *outCounts)
{
    int grandTotal = 0;
    for (int i = 0; i < count; i++)
        grandTotal += weights[i];

    orb_real_t shares[kMaxDrawingGroups];
    int assigned = 0;
    for (int i = 0; i < count; i++)
    {
        shares[i] = orb_real_t(total) * orb_real_t(weights[i]) / orb_real_t(grandTotal);
        outCounts[i] = int(shares[i]);
        assigned += outCounts[i];
    }
    int remainder = total - assigned;

    // Selection sort for the `remainder` largest fractional remainders -- count is at
    // most kMaxDrawingGroups (~40) and remainder < count, so this is cheap enough
    // without needing a real sort.
    bool used[kMaxDrawingGroups] = {};
    for (int r = 0; r < remainder; r++)
    {
        int best = -1;
        orb_real_t bestFrac = orb_real_t(-1);
        for (int i = 0; i < count; i++)
        {
            if (used[i])
                continue;
            orb_real_t frac = shares[i] - orb_real_t(outCounts[i]);
            if (frac > bestFrac)
            {
                bestFrac = frac;
                best = i;
            }
        }
        if (best >= 0)
        {
            outCounts[best]++;
            used[best] = true;
        }
    }
}

ElectronConfig buildAtomPointCloud(int z, AtomPoint *out, int count, uint32_t seed, AtomSubshellRange *outRanges,
                                   int *outRangeCount)
{
    // kAtomSizeCalibFactor[z-1] below only covers Z=1..92 (the display range cap, see
    // atom_cloud.h's docstring) even though slater.h's electronConfiguration() itself supports
    // up to kMaxZ=118.
    assert(z >= 1 && z <= kAtomSizeCalibCount && "buildAtomPointCloud: z outside the size-calibration table's range");
    ElectronConfig config = electronConfiguration(z);

    DrawingGroup groups[kMaxDrawingGroups];
    int groupCount = drawingGroups(config, groups);

    int weights[kMaxDrawingGroups] = {}; // zero-init: only [0, groupCount) is ever read, but
                                         // GCC's -Wmaybe-uninitialized can't prove that across
                                         // the splitCounts() call boundary otherwise
    for (int i = 0; i < groupCount; i++)
        weights[i] = groups[i].weight;
    int counts[kMaxDrawingGroups];
    splitCounts(weights, groupCount, count, counts);

    XorShift32 rng(seed);
    int idx = 0;
    int rangeCount = 0;
    for (int g = 0; g < groupCount; g++)
    {
        int n = groups[g].n, ell = groups[g].ell, m = groups[g].m;

        // Screened-potential (HFS/atomSFE) tables (data/hfs_tables.bin, read on demand from
        // the "storage" SPIFFS partition -- see hfs_radial.h's header comment) are the radial
        // model for z<=92 whenever available; the hydrogenic zEffRadial()/
        // buildRadialSamplerRuntime() path below is the fallback both for a z outside the
        // tables' NIST-verified 92/92 coverage AND for a board that hasn't run
        // `pio run -t uploadfs` yet (hfsFindU() returns nullptr in both cases).
        const orb_real_t *u = hfsFindU(z, n, ell);
        const RadialTable &radial = u != nullptr
                                        ? (m == kIsotropicM ? buildHfsRadialSamplerIsotropic(u)
                                                             : buildHfsRadialSamplerOriented(u))
                                        : buildRadialSamplerRuntime(n, ell, zEffRadial(z, config, n, ell));

        int groupStart = idx;
        if (m == kIsotropicM)
        {
            for (int i = 0; i < counts[g]; i++)
            {
                OrbitalPoint p = sampleIsotropicPoint(radial, &rng);
                out[idx++] = {p.x, p.y, p.z};
            }
        }
        else
        {
            const OrbitalAngularTables *angular = findAngularTables(ell, m);
            assert(angular != nullptr && "findAngularTables: (ell,m) missing from kAngularLibraryDescriptors "
                                          "for a value drawingGroups()/slater.h produced");
            for (int i = 0; i < counts[g]; i++)
            {
                OrbitalPoint p = sampleOrientedPoint(radial, *angular, &rng);
                out[idx++] = {p.x, p.y, p.z};
            }
        }

        // Drawing groups from a Hund's-rule partial fill (several per subshell, one per
        // occupied m) are always sampled back to back for the same subshellIndex -- merge them
        // into that subshell's single AtomSubshellRange instead of one range per drawing group.
        if (g > 0 && groups[g].subshellIndex == groups[g - 1].subshellIndex)
            outRanges[rangeCount - 1].count += counts[g];
        else
            outRanges[rangeCount++] = {n, ell, config.subshells[groups[g].subshellIndex].occ, groupStart, counts[g]};
    }
    *outRangeCount = rangeCount;

    // Clementi-Raimondi size calibration (see atom_size_calib.h): scale the
    // whole cloud so the valence subshell's mode radius lands on the
    // literature value, keeping the internal shell structure and relative
    // sizes. The device zoom-to-fits via kAtomTargetPx/rRef (scaleForAtom),
    // so on-device this mainly makes the pm scale bar and the dissection
    // zoom depths CR-consistent. kAtomSizeCalibFactor is table-based here
    // (CR / HFS-table valence mode, tools/atom_size_calib_gen.py's
    // compute_table_factors() -- see pc/RUN_HFS.md's device note), matching
    // the radial model actually rendered above; the internal shell
    // structure past the valence subshell stays the LDA tables' own (see
    // pc/RUN_HFS.md section 5's SIE/relativistic-offset caveats).
    orb_real_t calib = kAtomSizeCalibFactor[z - 1];
    if (calib != orb_real_t(1))
        for (int i = 0; i < idx; i++)
        {
            out[i].x *= calib;
            out[i].y *= calib;
            out[i].z *= calib;
        }

    return config;
}

const uint8_t *shellBaseRgb(int n)
{
    return (n >= 1 && n <= 7) ? kAtomShellRgb[n] : kAtomShellRgb[8];
}

namespace
{
    // Static, not stack-local: see orbital_presets.cpp's computeOrbitalLevels() comment --
    // this project avoids large stack arrays after a real task stack overflow. Shared by
    // outerSubshellRRef() and subshellDissectionPlan() below (never called concurrently or
    // re-entrantly -- both only ever run on the main render task) rather than each keeping
    // its own kAtomNumPoints-sized copy: on real hardware, unrelated extra static buffers
    // added while building the dissection view already starved Display::Display()'s frame
    // buffer allocation once (see atom_view.cpp's AtomPresetState comment for the bigger
    // instance of this same lesson) -- not worth risking a repeat over one harmless-looking
    // duplicate static array.
    EXT_RAM_BSS_ATTR orb_real_t sSubshellRadii[kAtomNumPoints];
} // namespace

/// p90 radius of `points[startIndex, startIndex+count)`, via sSubshellRadii scratch. Shared by
/// outerSubshellRRef()/subshellDissectionPlan() below -- both just measure a different set of
/// ranges over the same underlying computation.
static orb_real_t p90RadiusOfRange(const AtomPoint *points, int startIndex, int count)
{
    orb_real_t *radii = sSubshellRadii;
    for (int i = 0; i < count; i++)
    {
        orb_real_t x = points[startIndex + i].x, y = points[startIndex + i].y, z = points[startIndex + i].z;
        radii[i] = std::sqrt(x * x + y * y + z * z);
    }
    int idx = int(orb_real_t(0.90) * orb_real_t(count - 1));
    if (idx >= count)
        idx = count - 1;
    // Only radii[idx] itself is ever read -- nth_element partitions it into place in O(count)
    // instead of paying for a full O(count log count) sort.
    std::nth_element(radii, radii + idx, radii + count);
    return radii[idx] > orb_real_t(1e-6) ? radii[idx] : orb_real_t(1);
}

OuterSubshell outerSubshellRRef(const AtomPoint *points, const AtomSubshellRange *ranges, int rangeCount)
{
    OuterSubshell best;
    orb_real_t bestR = orb_real_t(-1);
    for (int s = 0; s < rangeCount; s++)
    {
        const AtomSubshellRange &r = ranges[s];
        if (r.count == 0)
            continue;
        orb_real_t rRef = p90RadiusOfRange(points, r.startIndex, r.count);
        if (rRef > bestR)
        {
            bestR = rRef;
            best = {r.n, r.ell, rRef};
        }
    }
    return best;
}

int subshellDissectionPlan(const AtomPoint *points, const AtomSubshellRange *ranges, int rangeCount,
                           DissectionEntry *out)
{
    int written = 0;
    for (int s = 0; s < rangeCount; s++)
    {
        const AtomSubshellRange &r = ranges[s];
        if (r.count == 0)
            continue;
        orb_real_t rRef = p90RadiusOfRange(points, r.startIndex, r.count);
        out[written++] = {r.n, r.ell, rRef, r.occ, r.startIndex, r.count};
    }

    // Small array (<= kMaxConfigSubshells, at most ~20 entries) -- insertion sort descending
    // by radius is simplest and plenty fast here, no need for std::sort + a comparator.
    for (int i = 1; i < written; i++)
    {
        DissectionEntry key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rRef < key.rRef)
        {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return written;
}

/// @brief apply color brighten factor to a single RGB channel, clamping to [0,255] and rounding to nearest integer.
/// @param c the color to brighten (0..255)
/// @param factor the brighten factor (0..1) -- 0 means no change, 1 means full brighten/dim
/// @return the brightened color (0..255)
static uint8_t brightenChannel(uint8_t c, orb_real_t factor)
{
    return uint8_t(orb_real_t(c) + (orb_real_t(255) - orb_real_t(c)) * factor);
}

/// @brief apply color dim factor to a single RGB channel, clamping to [0,255] and rounding to nearest integer.
/// @param c the color to dim (0..255)
/// @param factor the dim factor (0..1) -- 0 means no change, 1 means full brighten/dim
/// @return the dimmed color (0..255)
static uint8_t dimChannel(uint8_t c, orb_real_t factor)
{
    return uint8_t(orb_real_t(c) * (orb_real_t(1) - factor));
}

void colorizeAtomSubshells(const AtomSubshellRange *ranges, int rangeCount, const OuterSubshell &outer,
                           uint16_t *outColors)
{
    for (int i = 0; i < rangeCount; i++)
    {
        const uint8_t *base = shellBaseRgb(ranges[i].n);
        bool isOuter = ranges[i].n == outer.n && ranges[i].ell == outer.ell;
        uint8_t r, g, b;
        if (isOuter)
        {
            r = brightenChannel(base[0], kAtomOuterShellBrighten);
            g = brightenChannel(base[1], kAtomOuterShellBrighten);
            b = brightenChannel(base[2], kAtomOuterShellBrighten);
        }
        else
        {
            r = dimChannel(base[0], kAtomInnerShellDim);
            g = dimChannel(base[1], kAtomInnerShellDim);
            b = dimChannel(base[2], kAtomInnerShellDim);
        }
        outColors[i] = Display::packColor565(r, g, b);
    }
}

AtomScale scaleForAtom(orb_real_t rRef, orb_real_t amplitudeFraction)
{
    orb_real_t baseScale = kAtomTargetPx / rRef;
    return AtomScale{baseScale, baseScale * amplitudeFraction, rRef};
}
