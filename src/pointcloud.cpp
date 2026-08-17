// buildInverseCdf()/buildOrbitalSamplerConstexpr()/initOrbitalSampler() are
// now header-only constexpr/inline (see pointcloud.h) so a fixed set of
// orbitals' sampler tables can be embedded as compile-time .rodata. Only the
// genuinely-runtime pieces remain here: RNG state advancement and the
// per-point sample draw, neither of which has anything to precompute.
#include "pointcloud.h"

#include <cmath>

uint32_t XorShift32::next()
{
    uint32_t x = state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state = x;
    return x;
}

orb_real_t XorShift32::uniform01()
{
    return orb_real_t(next()) / orb_real_t(4294967296.0); // 2^32
}

OrbitalPoint sampleOrbitalPoint(const OrbitalSampler *sampler, XorShift32 *rng)
{
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
