#include "pointcloud.h"

#include <cmath>

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

namespace {

// Given non-negative sample weights of a density over [0, domainMax] taken
// at `count` evenly spaced points, build the inverse CDF: invTable[k] is the
// x-value at quantile k/(count-1). weight[] is overwritten in place (used as
// scratch space for the running cumulative sum) -- callers don't need it
// afterwards. Both the forward cumulative sum and the inverse lookup below
// are single monotonic sweeps (the CDF is non-decreasing in i, and the
// target quantile u is non-decreasing in k), so this is O(count) total, not
// O(count log count) -- no per-point search survives into sampling either,
// since invTable is later read directly via getValueFromLookupTable().
void buildInverseCdf(orb_real_t* weight, int count, orb_real_t domainMax, orb_real_t* invTable) {
    orb_real_t delta = domainMax / orb_real_t(count - 1);

    orb_real_t cumulative = orb_real_t(0);
    for (int i = 0; i < count; i++) {
        cumulative += weight[i];
        weight[i] = cumulative; // weight[] now holds the (unnormalized) CDF
    }
    orb_real_t total = weight[count - 1];
    if (total <= orb_real_t(0))
        total = orb_real_t(1); // degenerate guard; shouldn't occur for valid quantum numbers
    for (int i = 0; i < count; i++)
        weight[i] /= total;

    int j = 0;
    for (int k = 0; k < count; k++) {
        orb_real_t u = orb_real_t(k) / orb_real_t(count - 1);
        while (j < count - 1 && weight[j] < u)
            j++;
        int j0 = j > 0 ? j - 1 : 0;
        int j1 = j;
        orb_real_t c0 = weight[j0];
        orb_real_t c1 = weight[j1];
        orb_real_t t = (c1 > c0) ? (u - c0) / (c1 - c0) : orb_real_t(0);
        invTable[k] = (orb_real_t(j0) + t * orb_real_t(j1 - j0)) * delta;
    }
}

} // namespace

void initOrbitalSampler(OrbitalSampler* sampler, int n, int ell, int m) {
    sampler->n = n;
    sampler->ell = ell;
    sampler->m = m;

    orb_real_t radialCoeff[kOrbitalNMax];
    laguerreCoeffs(n, ell, radialCoeff);
    orb_real_t legendreCoeff[kOrbitalEllMax + 1];
    legendreCoeffs(ell, m, legendreCoeff);

    orb_real_t maxR = orb_real_t(6 * n * n);
    sampler->maxR = maxR;

    {
        orb_real_t weight[kOrbitalTableSize];
        orb_real_t deltaR = maxR / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++) {
            orb_real_t r = orb_real_t(i) * deltaR;
            orb_real_t R = hydrogenRadialFunction(r, n, ell, radialCoeff);
            weight[i] = (r * R) * (r * R);
        }
        buildInverseCdf(weight, kOrbitalTableSize, maxR, sampler->invRTable);
    }
    {
        orb_real_t weight[kOrbitalTableSize];
        orb_real_t deltaTheta = kOrbitalPi / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++) {
            orb_real_t theta = orb_real_t(i) * deltaTheta;
            orb_real_t P = computePLM(theta, ell, m, legendreCoeff);
            weight[i] = P * P * std::sin(theta);
        }
        buildInverseCdf(weight, kOrbitalTableSize, kOrbitalPi, sampler->invThetaTable);
    }
    {
        orb_real_t weight[kOrbitalTableSize];
        orb_real_t twoPi = orb_real_t(2) * kOrbitalPi;
        orb_real_t deltaPhi = twoPi / orb_real_t(kOrbitalTableSize - 1);
        for (int i = 0; i < kOrbitalTableSize; i++) {
            orb_real_t phi = orb_real_t(i) * deltaPhi;
            orb_real_t azimuthal = (m >= 0) ? std::cos(orb_real_t(m) * phi) : std::sin(orb_real_t(-m) * phi);
            weight[i] = azimuthal * azimuthal; // m==0 -> constant 1, degenerates to a uniform phi distribution
        }
        buildInverseCdf(weight, kOrbitalTableSize, twoPi, sampler->invPhiTable);
    }
}

OrbitalPoint sampleOrbitalPoint(const OrbitalSampler* sampler, XorShift32* rng) {
    orb_real_t r = getValueFromLookupTable(rng->uniform01(), sampler->invRTable, kOrbitalTableSize);
    orb_real_t theta = getValueFromLookupTable(rng->uniform01(), sampler->invThetaTable, kOrbitalTableSize);
    orb_real_t phi = getValueFromLookupTable(rng->uniform01(), sampler->invPhiTable, kOrbitalTableSize);

    orb_real_t sinTheta = std::sin(theta);
    OrbitalPoint pt;
    pt.x = r * sinTheta * std::cos(phi);
    pt.y = r * sinTheta * std::sin(phi);
    pt.z = r * std::cos(theta);
    return pt;
}
