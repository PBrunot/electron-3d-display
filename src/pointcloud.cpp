#include "pointcloud.h"

#include <cmath>

namespace {
// Multiplies the tabulated peak-magnitude bound by a small margin, since the
// true continuous peak of a smooth function can fall between two of the
// kOrbitalTableSize tabulated points. Not a rigorous guarantee, but ample
// for a point-cloud visualization (same pragmatic-approximation spirit as
// the "no z-buffer, approximate painter's sort" choices in CLAUDE.md §5.3).
// Mirrored exactly (same literal) in micropython/pointcloud.py and
// tools/orbitals_host/js_reference.js.
constexpr orb_real_t kDensityBoundMargin = orb_real_t(1.15);
} // namespace

uint32_t XorShift32::next() {
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

orb_real_t XorShift32::uniform01() {
    return orb_real_t(next()) / orb_real_t(4294967296.0); // 2^32
}

void initOrbitalSampler(OrbitalSampler* sampler, int n, int ell, int m) {
    sampler->n = n;
    sampler->ell = ell;
    sampler->m = m;
    laguerreCoeffs(n, ell, sampler->radialCoeff);
    legendreCoeffs(ell, m, sampler->legendreCoeff);

    orb_real_t radialTable[kOrbitalTableSize];
    orb_real_t maxR = orb_real_t(0);
    buildRadialTable(n, ell, radialTable, kOrbitalTableSize, &maxR);
    sampler->maxR = maxR;
    orb_real_t deltaR = maxR / orb_real_t(kOrbitalTableSize - 1);
    orb_real_t maxRr = orb_real_t(0);
    for (int i = 0; i < kOrbitalTableSize; i++) {
        orb_real_t rr = std::abs(radialTable[i] * (orb_real_t(i) * deltaR));
        if (rr > maxRr)
            maxRr = rr;
    }

    orb_real_t legendreTable[kOrbitalTableSize];
    buildLegendreTable(ell, m, legendreTable, kOrbitalTableSize);
    orb_real_t maxP = orb_real_t(0);
    for (int i = 0; i < kOrbitalTableSize; i++) {
        orb_real_t p = std::abs(legendreTable[i]);
        if (p > maxP)
            maxP = p;
    }

    sampler->densityBound = maxRr * maxRr * maxP * maxP * kDensityBoundMargin;
}

OrbitalPoint sampleOrbitalPoint(const OrbitalSampler* sampler, XorShift32* rng, int maxAttempts) {
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        // Fixed draw order (r, theta, phi, u) -- mirrored exactly in the
        // MicroPython and JS ports so identical seeds produce identical
        // accept/reject sequences and identical accepted points.
        orb_real_t r = rng->uniform01() * sampler->maxR;
        orb_real_t theta = rng->uniform01() * kOrbitalPi;
        orb_real_t phi = rng->uniform01() * (orb_real_t(2) * kOrbitalPi);
        orb_real_t u = rng->uniform01() * sampler->densityBound;

        orb_real_t psi = psiReal(r, theta, phi, sampler->n, sampler->ell, sampler->m, sampler->radialCoeff,
                                  sampler->legendreCoeff);
        orb_real_t density = (psi * r) * (psi * r) * std::sin(theta);

        if (u <= density) {
            orb_real_t sinTheta = std::sin(theta);
            OrbitalPoint pt;
            pt.x = r * sinTheta * std::cos(phi);
            pt.y = r * sinTheta * std::sin(phi);
            pt.z = r * std::cos(theta);
            return pt;
        }
    }
    return OrbitalPoint{orb_real_t(0), orb_real_t(0), orb_real_t(0)};
}
