#include "pointcloud.h"

#include <cmath>

namespace {
// Multiplies each tabulated peak-magnitude bound by a small margin, since the
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
    orb_real_t maxRWeight = orb_real_t(0);
    for (int i = 0; i < kOrbitalTableSize; i++) {
        orb_real_t rr = radialTable[i] * (orb_real_t(i) * deltaR);
        orb_real_t w = rr * rr;
        if (w > maxRWeight)
            maxRWeight = w;
    }
    sampler->maxRWeight = maxRWeight * kDensityBoundMargin;

    orb_real_t legendreTable[kOrbitalTableSize];
    buildLegendreTable(ell, m, legendreTable, kOrbitalTableSize);
    orb_real_t deltaTheta = kOrbitalPi / orb_real_t(kOrbitalTableSize - 1);
    orb_real_t maxThetaWeight = orb_real_t(0);
    for (int i = 0; i < kOrbitalTableSize; i++) {
        orb_real_t theta = orb_real_t(i) * deltaTheta;
        orb_real_t w = legendreTable[i] * legendreTable[i] * std::sin(theta);
        if (w > maxThetaWeight)
            maxThetaWeight = w;
    }
    sampler->maxThetaWeight = maxThetaWeight * kDensityBoundMargin;
}

namespace {

// Rejection-sample r against the r-marginal density [r*R(r)]^2.
orb_real_t sampleR(const OrbitalSampler* s, XorShift32* rng, int maxAttempts) {
    orb_real_t r = orb_real_t(0);
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        r = rng->uniform01() * s->maxR;
        orb_real_t u = rng->uniform01() * s->maxRWeight;
        orb_real_t R = hydrogenRadialFunction(r, s->n, s->ell, s->radialCoeff);
        orb_real_t weight = (r * R) * (r * R);
        if (u <= weight)
            break;
    }
    return r;
}

// Rejection-sample theta against the theta-marginal density P_l^m(theta)^2*sin(theta).
orb_real_t sampleTheta(const OrbitalSampler* s, XorShift32* rng, int maxAttempts) {
    orb_real_t theta = orb_real_t(0);
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        theta = rng->uniform01() * kOrbitalPi;
        orb_real_t u = rng->uniform01() * s->maxThetaWeight;
        orb_real_t P = computePLM(theta, s->ell, s->m, s->legendreCoeff);
        orb_real_t weight = P * P * std::sin(theta);
        if (u <= weight)
            break;
    }
    return theta;
}

// Sample phi: uniform directly when m==0 (azimuthal factor is constant 1, no
// rejection needed), otherwise rejection-sample against cos^2(m*phi) /
// sin^2(m*phi) (envelope 1, so this needs ~2 attempts on average).
orb_real_t samplePhi(const OrbitalSampler* s, XorShift32* rng, int maxAttempts) {
    orb_real_t phi = rng->uniform01() * (orb_real_t(2) * kOrbitalPi);
    if (s->m == 0)
        return phi;
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        phi = rng->uniform01() * (orb_real_t(2) * kOrbitalPi);
        orb_real_t u = rng->uniform01();
        orb_real_t azimuthal = (s->m >= 0) ? std::cos(orb_real_t(s->m) * phi) : std::sin(orb_real_t(-s->m) * phi);
        if (u <= azimuthal * azimuthal)
            break;
    }
    return phi;
}

} // namespace

OrbitalPoint sampleOrbitalPoint(const OrbitalSampler* sampler, XorShift32* rng, int maxAttempts) {
    orb_real_t r = sampleR(sampler, rng, maxAttempts);
    orb_real_t theta = sampleTheta(sampler, rng, maxAttempts);
    orb_real_t phi = samplePhi(sampler, rng, maxAttempts);

    orb_real_t sinTheta = std::sin(theta);
    OrbitalPoint pt;
    pt.x = r * sinTheta * std::cos(phi);
    pt.y = r * sinTheta * std::sin(phi);
    pt.z = r * std::cos(theta);
    return pt;
}
