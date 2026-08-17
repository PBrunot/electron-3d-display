#include "orbital_presets.h"

#include <algorithm>
#include <cmath>

#include "display.h"         // packColor565
#include "orbital_library.h" // findOrbitalSampler

constexpr int kOrbitalColorMinLevel = 60; // keeps the dimmest points visible instead of fading to black

// Phase colors -- chemistry-diagram convention: positive lobe warm (red/orange), negative
// lobe cool (blue/cyan). s orbitals (ell=0) are single-signed everywhere, so they render
// as a uniform warm cloud with no visible split -- expected, not a bug (no phase change
// without a node).
static constexpr uint8_t kPhasePositiveRgb[3] = {255, 120, 40};
static constexpr uint8_t kPhaseNegativeRgb[3] = {40, 120, 255};

uint16_t orbitalLevelToColor565(int level, int sign)
{
    const uint8_t *base = sign >= 0 ? kPhasePositiveRgb : kPhaseNegativeRgb;
    return Display::packColor565(uint8_t(base[0] * level / 255), uint8_t(base[1] * level / 255),
                                 uint8_t(base[2] * level / 255));
}

void computeOrbitalLevels(const orb_real_t *psi2, int count, uint8_t *outLevels, orb_real_t *outPsi2Sorted)
{
    // Static, not stack-local: this project avoids large stack arrays after
    // pointcloud.h's buildRadialSamplerRuntime() already hit a real task stack overflow
    // with a much smaller (~4KB) local array.
    static int order[kOrbitalMaxPoints];
    for (int i = 0; i < count; i++)
        order[i] = i;
    std::sort(order, order + count, [psi2](int a, int b)
              { return psi2[a] < psi2[b]; });

    int span = kOrbitalColorMaxLevel - kOrbitalColorMinLevel;
    int denom = count > 1 ? count - 1 : 1;
    for (int rank = 0; rank < count; rank++)
    {
        int i = order[rank];
        outPsi2Sorted[rank] = psi2[i];
        outLevels[i] = uint8_t(kOrbitalColorMinLevel + (rank * span) / denom);
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
    int span = kOrbitalColorMaxLevel - kOrbitalColorMinLevel;
    int sortedSpan = state->count > 1 ? state->count - 1 : 1;
    int level = kOrbitalColorMinLevel + (rank * span) / sortedSpan;
    int sign = psi >= orb_real_t(0) ? 1 : -1;

    return ResampledOrbitalPoint{idx, level, sign};
}

// Projection scale target: the p90-radius point should land kOrbitalP90TargetPx out from
// center. Measured per preset (scaleFromRadii()) rather than a global constant, since
// radial extent depends on n, l, AND m, not n alone.
static constexpr orb_real_t kOrbitalP90TargetPx = orb_real_t(100);
static constexpr orb_real_t kOrbitalZoomAmplitudeFraction = orb_real_t(0.4);

OrbitalScale scaleFromRadii(const OrbitalPoint *points, int count)
{
    static orb_real_t radii[kOrbitalMaxPoints]; // static scratch, see computeOrbitalLevels()'s comment
    for (int i = 0; i < count; i++)
        radii[i] = std::sqrt(points[i].x * points[i].x + points[i].y * points[i].y + points[i].z * points[i].z);
    std::sort(radii, radii + count);

    int idx = int(orb_real_t(0.90) * orb_real_t(count - 1));
    if (idx >= count)
        idx = count - 1;
    orb_real_t rRef = radii[idx] > orb_real_t(1e-6) ? radii[idx] : orb_real_t(1);
    orb_real_t baseScale = kOrbitalP90TargetPx / rRef;
    return OrbitalScale{baseScale, baseScale * kOrbitalZoomAmplitudeFraction, rRef};
}

void buildOrbitalPointCloud(int n, int ell, int m, OrbitalPoint *outPoints, orb_real_t *outPsi2, int8_t *outSigns,
                            int count, uint32_t seed, XorShift32 *outRng, orb_real_t *outRadialCoeff,
                            orb_real_t *outLegendreCoeff)
{
    const OrbitalSampler *sampler = findOrbitalSampler(n, ell, m);
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
