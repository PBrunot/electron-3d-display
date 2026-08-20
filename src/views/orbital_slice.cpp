#include "views/orbital_slice.h"

#include <algorithm>
#include <cmath>

#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "physics/orbital_presets.h" // orbitalLevelToColor565, kOrbitalColorMaxLevel
#include "render/display.h"          // Display::kColorOrbitalRed/Blue, kDisplayWidth
#include "render/overlay.h"          // kPmPerBohr

namespace
{
    constexpr int kSliceCellCount = kSliceGridSize * kSliceGridSize;

    // One-time-build scratch (|cell value| magnitudes for the v99 percentile selection, and
    // the index order for nth_element -- see buildSliceTable()) -- static PSRAM, not stack,
    // same rationale as orbital_presets.cpp's own computeOrbitalLevels() scratch (order[]):
    // this project avoids large stack arrays after a real task stack overflow with a much
    // smaller one (see pointcloud.h's buildRadialSamplerRuntime()).
    EXT_RAM_BSS_ATTR orb_real_t sliceMag[kSliceCellCount];
    EXT_RAM_BSS_ATTR int sliceOrder[kSliceCellCount];
} // namespace

void buildSliceTable(int n, int ell, int m, const orb_real_t *radialCoeff, const orb_real_t *legendreCoeff,
                     orb_real_t rRef, SliceTable *out)
{
    out->n = n;
    out->ell = ell;
    out->m = m;

    int absM = m < 0 ? -m : m;
    // Lobe-plane azimuth ph0 (SLICE.md section 3): 0 for cos-type (m >= 0), pi/(2|m|) for
    // sin-type (m < 0) -- both choices make A(m, ph0) == 1 by construction, so the azimuthal
    // factor below is never evaluated at render time, just folded into a +-1 sign flip.
    out->planeAzimuth = (m >= 0) ? orb_real_t(0) : kOrbitalPi / orb_real_t(2 * absM);
    // (-1)^|m|: the sign the x < 0 half of the plane picks up relative to the x > 0 half, per
    // the identity A(ph0 + pi) = (-1)^|m| * A(ph0) (SLICE.md section 3).
    int negHalfSign = (absM % 2 == 0) ? 1 : -1;

    out->extentBohr = kSliceFramingFactor * rRef;
    out->extentPm = out->extentBohr * kPmPerBohr;
    orb_real_t cellBohr = (out->extentBohr * orb_real_t(2)) / orb_real_t(kSliceGridSize);

    for (int row = 0; row < kSliceGridSize; row++)
    {
        // Row 0 is the top of the screen -- map it to +z, the bottom row to -z, matching the
        // reference repo's plot orientation (SLICE.md's sanity checks assume this).
        orb_real_t z = out->extentBohr - (orb_real_t(row) + orb_real_t(0.5)) * cellBohr;
        for (int col = 0; col < kSliceGridSize; col++)
        {
            orb_real_t x = -out->extentBohr + (orb_real_t(col) + orb_real_t(0.5)) * cellBohr;
            orb_real_t r = std::sqrt(x * x + z * z);
            orb_real_t cosTheta = r > orb_real_t(1e-9) ? std::clamp(z / r, orb_real_t(-1), orb_real_t(1)) : orb_real_t(0);
            orb_real_t theta = std::acos(cosTheta);

            orb_real_t base = hydrogenRadialFunction(r, n, ell, radialCoeff) * computePLM(theta, ell, m, legendreCoeff);
            orb_real_t value = (x < orb_real_t(0)) ? base * orb_real_t(negHalfSign) : base;

            int idx = row * kSliceGridSize + col;
            out->sign[idx] = value >= orb_real_t(0) ? int8_t(1) : int8_t(-1);
            sliceMag[idx] = value < orb_real_t(0) ? -value : value;
        }
    }

    // Density-based (NOT rank) normalization -- see SLICE.md's contrast note. Rank-equalizing
    // a uniform grid lights half the screen to ~83% brightness by construction (median level
    // 212 for every orbital, 0% dark cells): unlike the 3D cloud's density-weighted point
    // samples, a grid has no empty screen space to stay black. Normalizing each cell's |psi|^2
    // against the grid's own 99.9th percentile (self-normalizing per orbital, same convention
    // as the reference repo's vmax) with a gamma lift preserves the real density falloff:
    //   level = 255 * min(1, |psi|^2 / v99)^kSliceLevelGamma
    // Bright lobe cores, near-black tails. v99 via nth_element on the index array (keeps
    // sliceMag's per-cell values intact for the level pass) -- cheaper than a full sort.
    for (int i = 0; i < kSliceCellCount; i++)
        sliceOrder[i] = i;
    int v99Index = int(kSliceDensityPercentile * orb_real_t(kSliceCellCount - 1));
    std::nth_element(sliceOrder, sliceOrder + v99Index, sliceOrder + kSliceCellCount,
                     [](int a, int b) { return sliceMag[a] < sliceMag[b]; });
    orb_real_t v99 = sliceMag[sliceOrder[v99Index]];
    if (v99 <= orb_real_t(0))
        v99 = orb_real_t(1);

    for (int i = 0; i < kSliceCellCount; i++)
    {
        orb_real_t density = sliceMag[i] * sliceMag[i] / v99;
        if (density > orb_real_t(1))
            density = orb_real_t(1);
        orb_real_t lv = std::pow(density, kSliceLevelGamma) * orb_real_t(kOrbitalColorMaxLevel);
        out->level[i] = uint8_t(lv >= orb_real_t(255) ? 255 : int(lv));
    }
}

void renderSliceFrame(uint16_t *frameBuf, const SliceTable &t, orb_real_t fade)
{
    for (int row = 0; row < kSliceGridSize; row++)
    {
        for (int col = 0; col < kSliceGridSize; col++)
        {
            int idx = row * kSliceGridSize + col;
            int level = int(orb_real_t(t.level[idx]) * fade);
            uint16_t color =
                orbitalLevelToColor565(level, t.sign[idx], Display::kColorOrbitalRed, Display::kColorOrbitalBlue);

            int px0 = col * kSliceCellPx;
            int py0 = row * kSliceCellPx;
            for (int py = py0; py < py0 + kSliceCellPx; py++)
            {
                int rowBase = py * Display::kDisplayWidth;
                for (int px = px0; px < px0 + kSliceCellPx; px++)
                    frameBuf[rowBase + px] = color;
            }
        }
    }
}
