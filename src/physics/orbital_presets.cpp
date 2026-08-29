#include "physics/orbital_presets.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "render/display.h"          // packColor565
#include "esp_attr.h"                // EXT_RAM_BSS_ATTR
#include "physics/orbital_library.h" // findOrbitalSampler

// ============================================================================================
// Tunable constants
// ============================================================================================

// kOrbitalColorMinLevel/kOrbitalLevelGamma/orbitalLevelFromRankFraction() now live in
// orbital_presets.h (public) -- see that header's comment.

/// Projection scale target: the p90-radius point should land this far out from center, in
/// pixels. Measured per preset (scaleFromRadii()) rather than applied as a flat constant,
/// since radial extent depends on n, l, AND m, not n alone.
static constexpr orb_real_t kOrbitalP90TargetPx = orb_real_t(100);
static constexpr orb_real_t kOrbitalZoomAmplitudeFraction = orb_real_t(0.4);

// ============================================================================================

// Phase colors come from each preset's own pair (see orbital_library.h's
// OrbitalDescriptor.posRgb/negRgb); every preset uses the same orange/blue pair so sign of
// psi_real maps to color consistently across the whole library, rather than each orbital
// having its own distinguishing hue. s orbitals (ell=0) are single-signed everywhere, so they
// render as a uniform cloud in their positive (orange) color with no visible split --
// expected, not a bug (no phase change without a node).

uint16_t orbitalLevelToColor565(int level, int sign, const uint16_t posRgb565, const uint16_t negRgb565)
{
    const uint16_t base = sign >= 0 ? posRgb565 : negRgb565;
    uint8_t r = (base >> 11) & 0x1F;
    uint8_t g = (base >> 5) & 0x3F;
    uint8_t b = base & 0x1F;

    // Scale each channel by level/255, rounding to nearest integer.
    r = uint8_t((r * level + 127) / 255);
    g = uint8_t((g * level + 127) / 255);
    b = uint8_t((b * level + 127) / 255);

    return Display::packColor565(r << 3, g << 2, b << 3);
}

namespace
{
    // Shared scratch for computeOrbitalLevels()'s order[]/scaleFromRadii()'s radii[] below:
    // both are called sequentially within a single OrbitalPresetState::load() call (never
    // concurrently -- screenshot_pause.h's checkpoint() protocol serializes every load()
    // against any other reader/writer of this module's static scratch), and each function's
    // own use is fully self-contained (write, read, discard) within that one call -- so one
    // buffer, reinterpreted as whichever type is needed, safely replaces two separate
    // kOrbitalNumPoints-sized static arrays. Saves 4 bytes/point of internal SRAM on boards
    // with no PSRAM (CYD), where every such array falls back from EXT_RAM_BSS_ATTR's intended
    // PSRAM placement into that same tight budget -- see config/visual_constants.h's
    // kOrbitalNumPoints comment.
    union OrderRadiiScratch
    {
        int order[kOrbitalNumPoints];
        orb_real_t radii[kOrbitalNumPoints];
    };
    EXT_RAM_BSS_ATTR OrderRadiiScratch sOrderRadiiScratch;
} // namespace

void computeOrbitalLevels(const orb_real_t *psi2, int count, uint8_t *outLevels, orb_real_t *outPsi2Sorted)
{
    // Static, not stack-local: this project avoids large stack arrays after
    // pointcloud.h's buildRadialSamplerRuntime() already hit a real task stack overflow
    // with a much smaller (~4KB) local array.
    int *order = sOrderRadiiScratch.order;
    for (int i = 0; i < count; i++)
        order[i] = i;
    std::sort(order, order + count, [psi2](int a, int b)
              { return psi2[a] < psi2[b]; });

    int denom = count > 1 ? count - 1 : 1;
    for (int rank = 0; rank < count; rank++)
    {
        int i = order[rank];
        outPsi2Sorted[rank] = psi2[i];
        outLevels[i] = uint8_t(orbitalLevelFromRankFraction(float(rank) / float(denom)));
    }
}

/** Index where `value` would insert into ascending `sortedValues` -- manual binary search
 * (bisect_rank() in cloud_common.py; not the <algorithm> equivalent, to keep this a
 * standalone, easily-portable primitive matching every other port's version). */
static int bisectRank(const orb_real_t *sortedValues, int count, orb_real_t value)
{
    int lo = 0, hi = count;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (sortedValues[mid] < value)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

ResampledOrbitalPoint resampleOneOrbitalPoint(OrbitalResampleState *state, OrbitalPoint *points)
{
    int idx = state->cursor;
    state->cursor = (idx + 1 < state->count) ? idx + 1 : 0;

    OrbitalPoint p = sampleOrbitalPoint(state->sampler, &state->rng);
    points[idx] = p;

    orb_real_t r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    orb_real_t theta = r > orb_real_t(1e-9) ? std::acos(p.z / r) : orb_real_t(0);
    orb_real_t phi = std::atan2(p.y, p.x);
    orb_real_t psi =
        psiReal(r, theta, phi, state->n, state->ell, state->m, state->radialCoeff, state->legendreCoeff);
    orb_real_t psi2 = psi * psi;

    int rank = bisectRank(state->psi2Sorted, state->count, psi2);
    int sortedSpan = state->count > 1 ? state->count - 1 : 1;
    int level = orbitalLevelFromRankFraction(float(rank) / float(sortedSpan));
    int sign = psi >= orb_real_t(0) ? 1 : -1;

    return ResampledOrbitalPoint{idx, level, sign};
}

OrbitalScale scaleFromRadii(const OrbitalPoint *points, int count)
{
    static EXT_RAM_BSS_ATTR orb_real_t radii[kOrbitalNumPoints]; // static scratch, see computeOrbitalLevels()'s comment
    for (int i = 0; i < count; i++)
        radii[i] = std::sqrt(points[i].x * points[i].x + points[i].y * points[i].y + points[i].z * points[i].z);

    int idx = int(orb_real_t(0.90) * orb_real_t(count - 1));
    if (idx >= count)
        idx = count - 1;
    // Only radii[idx] itself is ever read -- nth_element partitions it into place in O(count)
    // instead of paying for a full O(count log count) sort (see atom_cloud.cpp's
    // p90RadiusOfRange() for the same change).
    std::nth_element(radii, radii + idx, radii + count);
    orb_real_t rRef = radii[idx] > orb_real_t(1e-6) ? radii[idx] : orb_real_t(1);
    orb_real_t baseScale = kOrbitalP90TargetPx / rRef;
    return OrbitalScale{baseScale, baseScale * kOrbitalZoomAmplitudeFraction, rRef};
}

void buildOrbitalPointCloud(int n, int ell, int m, OrbitalPoint *outPoints, orb_real_t *outPsi2, int8_t *outSigns,
                            int count, uint32_t seed, XorShift32 *outRng, orb_real_t *outRadialCoeff,
                            orb_real_t *outLegendreCoeff)
{
    const OrbitalSampler *sampler = findOrbitalSampler(n, ell, m);
    assert(sampler != nullptr && "findOrbitalSampler: (n,ell,m) missing from kOrbitalLibrary");
    XorShift32 rng(seed);
    laguerreCoeffs(n, ell, outRadialCoeff);
    legendreCoeffs(ell, m, outLegendreCoeff);

    for (int i = 0; i < count; i++)
    {
        OrbitalPoint p = sampleOrbitalPoint(sampler, &rng);
        outPoints[i] = p;

        orb_real_t r = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        orb_real_t theta = r > orb_real_t(1e-9) ? std::acos(p.z / r) : orb_real_t(0);
        orb_real_t phi = std::atan2(p.y, p.x);
        orb_real_t psi = psiReal(r, theta, phi, n, ell, m, outRadialCoeff, outLegendreCoeff);
        outPsi2[i] = psi * psi;
        outSigns[i] = psi >= orb_real_t(0) ? int8_t(1) : int8_t(-1);
    }
    *outRng = rng;
}
