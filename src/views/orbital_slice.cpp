#include "views/orbital_slice.h"

#include <algorithm>
#include <cmath>

#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "physics/orbital_presets.h" // kOrbitalColorMaxLevel
#include "render/display.h"          // Display::packColor565, kDisplayWidth
#include "render/overlay.h"          // kPmPerBohr

namespace
{
    constexpr int kSliceCellCount = kSliceGridSize * kSliceGridSize;

    // One-time-build scratch (|cell value| magnitudes for the v99 percentile selection, and
    // the index order for nth_element -- see buildSliceTable()) -- static PSRAM, not stack,
    // same rationale as orbital_presets.cpp's own computeOrbitalLevels() scratch (order[]):
    // this project avoids large stack arrays after a real task stack overflow with a much
    // smaller one (see pointcloud.h's buildRadialSamplerRuntime()). PSRAM-only (EXT_RAM_BSS_ATTR):
    // this whole file is excluded from the CYD build (see platformio.ini's [env:CYD]
    // build_src_filter and CYD-branch.md) precisely because these two arrays alone (~450KB at
    // the current 240x240 grid) would overflow that board's much smaller, PSRAM-less internal
    // RAM regardless of any point-cloud-count tuning -- not worth reworking into heap scratch
    // for a feature that's unreachable there anyway (Right-tilt-hold needs the IMU).
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
            // |value| only -- the sequential heatmap has no phase channel (see kSliceColormapStops).
            sliceMag[idx] = value < orb_real_t(0) ? -value : value;
        }
    }

    // Density-based (NOT rank) normalization -- see SLICE.md's contrast note. Rank-equalizing
    // a uniform grid lights half the screen to ~83% brightness by construction (median level
    // 212 for every orbital, 0% dark cells): unlike the 3D cloud's density-weighted point
    // samples, a grid has no empty screen space to stay black. Normalizing each cell's |psi|^2
    // against the grid's own 99.9th percentile (self-normalizing per orbital, same convention
    // as the reference repo's vmax) with a gamma lift preserves the real density falloff:
    //   level = 255 * min(1, |psi|^2 / v99)^gamma
    // Bright lobe cores, near-black tails. v99 via nth_element on the index array (keeps
    // sliceMag's per-cell values intact for the level pass) -- cheaper than a full sort.
    // nth_element picks the percentile of |psi| (sliceMag), not of |psi|^2 -- but squaring is
    // monotonic over non-negative values, so the percentile-of-|psi|, squared, equals the
    // percentile-of-|psi|^2 directly: v99 below IS the density (|psi|^2) percentile.
    for (int i = 0; i < kSliceCellCount; i++)
        sliceOrder[i] = i;
    int v99Index = int(kSliceDensityPercentile * orb_real_t(kSliceCellCount - 1));
    std::nth_element(sliceOrder, sliceOrder + v99Index, sliceOrder + kSliceCellCount,
                     [](int a, int b) { return sliceMag[a] < sliceMag[b]; });
    orb_real_t v99Mag = sliceMag[sliceOrder[v99Index]];
    orb_real_t v99 = v99Mag * v99Mag;
    if (v99 <= orb_real_t(0))
        v99 = orb_real_t(1);

    // Auto-exposure (see visual_constants.h's kSliceExposureScale comment for the rationale):
    // brightFraction = fraction of the visible cloud (density >= kSliceVisibleFrac*v99) that
    // is already near-max (density >= kSliceBrightFrac*v99). One counting pass, no extra
    // sorting.
    int visibleCount = 0, brightCount = 0;
    orb_real_t visibleFloor = kSliceVisibleFrac * v99;
    orb_real_t brightFloor = kSliceBrightFrac * v99;
    for (int i = 0; i < kSliceCellCount; i++)
    {
        orb_real_t density = sliceMag[i] * sliceMag[i];
        if (density >= visibleFloor)
        {
            visibleCount++;
            if (density >= brightFloor)
                brightCount++;
        }
    }
    orb_real_t brightFraction =
        visibleCount > 0 ? orb_real_t(brightCount) / orb_real_t(visibleCount) : orb_real_t(1);
    orb_real_t exposure = kSliceExposureScale * (orb_real_t(1) - brightFraction);
    if (exposure < orb_real_t(0))
        exposure = orb_real_t(0);
    orb_real_t gamma = orb_real_t(1) / (orb_real_t(1) + exposure);
    if (gamma < kSliceMinGamma)
        gamma = kSliceMinGamma;

    for (int i = 0; i < kSliceCellCount; i++)
    {
        orb_real_t density = sliceMag[i] * sliceMag[i] / v99;
        if (density > orb_real_t(1))
            density = orb_real_t(1);
        orb_real_t lv = std::pow(density, gamma) * orb_real_t(kOrbitalColorMaxLevel);
        out->level[i] = uint8_t(lv >= orb_real_t(255) ? 255 : int(lv));
    }
}

void renderSliceFrame(Display &display, const SliceTable &t, orb_real_t fade)
{
    // Full-resolution grid (kSliceGridSize == kDisplayWidth): one cell per pixel, so no 2x2
    // block writes and no interpolation -- each pixel is its own |psi|^2 sample, the
    // imshow-style look of the reference repo's plots.
    for (int i = 0; i < kSliceCellCount; i++)
    {
        int level = int(orb_real_t(t.level[i]) * fade);
        if (level > kOrbitalColorMaxLevel)
            level = kOrbitalColorMaxLevel;

        // Map the 0..255 level through the 16-stop sequential ramp (kSliceColormapStops),
        // per-channel linear interpolation between the two bracketing stops.
        orb_real_t pos = orb_real_t(level) / orb_real_t(kOrbitalColorMaxLevel) *
                         orb_real_t(kSliceColormapStopsCount - 1);
        int idx = int(pos);
        if (idx > kSliceColormapStopsCount - 2)
            idx = kSliceColormapStopsCount - 2;
        orb_real_t f = pos - orb_real_t(idx);
        const SliceColorStop &a = kSliceColormapStops[idx];
        const SliceColorStop &b = kSliceColormapStops[idx + 1];
        uint8_t r = uint8_t(orb_real_t(a.r) + (orb_real_t(b.r) - orb_real_t(a.r)) * f + orb_real_t(0.5));
        uint8_t g = uint8_t(orb_real_t(a.g) + (orb_real_t(b.g) - orb_real_t(a.g)) * f + orb_real_t(0.5));
        uint8_t bl = uint8_t(orb_real_t(a.b) + (orb_real_t(b.b) - orb_real_t(a.b)) * f + orb_real_t(0.5));
        display.writePx(i % kSliceGridSize, i / kSliceGridSize, Display::packColor565(r, g, bl));
    }
}
