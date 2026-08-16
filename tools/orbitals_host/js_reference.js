// Ground-truth reference for hydrogen orbital math, used to cross-check the
// C++ port in src/orbitals.cpp.
//
// This is a near-verbatim extraction of the orbital-math functions from
// quantum-physics.js (c) 2020-2022 Manuel Joffre, www.quantum-physics.polytechnique.fr
// (the full original file, which also contains unrelated double-slit-
// experiment / FFT / 2D-plotting code not needed here, is not redistributed
// in this repo -- see examples/js-calculations/README.md for where to get it).
// Function bodies below are copied unmodified from
// initLegendreCoeffs, computePLM, initLookupTable, initLaguerreCoeffs,
// hydrogenRadialFunction, initLookupTableRadial and getValueFromLookupTable —
// only the module-level `var` state they read/write has been kept alongside
// them (unchanged) so they still run standalone under Node, without the
// THREE.js/DOM code the rest of quantum-physics.js depends on.
//
// psiReal() at the bottom is NOT from the original file (it has no single
// function that combines R(r) and P_l^m(theta) into one signed 3D scalar
// field -- only |P_l^m(theta)*cos|sin(m*phi)| for surface-plot radii). It
// mirrors psiReal() in src/orbitals.cpp so the two can be cross-checked
// end-to-end, not just their ingredients in isolation.

'use strict';

// ---- module-level state (verbatim names from quantum-physics.js) ----

var legendreCoeff = Array(64);
var laguerreCoeff = Array(64);

var tabulatedN = -1;
var tabulatedEll = -1;

// ---- verbatim port of initLegendreCoeffs(ell, m) ----

function initLegendreCoeffs(ell, m) {
    var absM = Math.abs(m);
    var ellEll1 = ell * (ell + 1);
    legendreCoeff[0] = 1 - 2 * (ell % 2);
    for (var iM = ell; iM > absM; iM--) {
        var denominator = Math.sqrt(ellEll1 - iM * (iM - 1));
        var kStart = (ell - iM) % 2;
        if (kStart == 1)
            legendreCoeff[0] = 0;
        for (var k = kStart; k <= (ell - iM); k += 2) {
            if (k > 0)
                legendreCoeff[k - 1] += k * legendreCoeff[k] / denominator;
            legendreCoeff[k + 1] = -(k + 2 * iM) * legendreCoeff[k] / denominator;
        }
    }
    // Normalization so that maximum value is equal to one
    var maxValue = 0;
    for (var theta = 0; theta < Math.PI / 2; theta += Math.PI / 100)
        maxValue = Math.max(maxValue, Math.abs(computePLM(theta, ell, m)));
    for (var iM = 0; iM <= ell; iM++) {
        legendreCoeff[iM] /= maxValue;
    }
}

// ---- verbatim port of computePLM(theta, ell, m) ----

function computePLM(theta, ell, m) {
    var u = Math.cos(theta);
    var absM = Math.abs(m);
    var sum = 0;
    var uPowJ;
    if ((ell - absM) % 2 == 0)
        uPowJ = 1;
    else
        uPowJ = u;
    var u2 = u * u;
    for (var j = (ell - absM) % 2; j <= ell - absM; j += 2, uPowJ *= u2)
        sum += legendreCoeff[j] * uPowJ;
    return sum * Math.pow(Math.sin(theta), Math.abs(m));
}

// ---- verbatim port of initLookupTable(ell, m), parameterized on table size ----

function initLookupTable(ell, m, nTable) {
    var table = Array(nTable);
    for (var i = 0; i < nTable; i++) {
        var theta = Math.PI * i / (nTable - 1);
        table[i] = computePLM(theta, ell, m);
    }
    return table;
}

// ---- verbatim port of initLaguerreCoeffs(n, ell) ----

function initLaguerreCoeffs(n, ell) {
    tabulatedN = n;
    tabulatedEll = ell;
    for (var k = 0; k < laguerreCoeff.length; k++) {
        laguerreCoeff[k] = 0;
    }
    var nMax = laguerreCoeff.length;
    n = Math.min(n, nMax);
    ell = Math.min(ell, n - 1);
    ell = Math.max(ell, 0);
    var degree = n - ell;
    laguerreCoeff[0] = 1;
    for (var k = 0; k + 1 < degree; k++)
        laguerreCoeff[k + 1] = -2 * (1 - (ell + k + 1.) / n) / (k + 1.) / (2 * ell + k + 2) * laguerreCoeff[k];
}

// ---- verbatim port of hydrogenRadialFunction(r, n, ell) ----
// (caching via tabulatedN/tabulatedEll kept, matching the original)

function hydrogenRadialFunction(r, n, ell) {
    if ((n !== tabulatedN) || (ell !== tabulatedEll))
        initLaguerreCoeffs(n, ell);
    var result = laguerreCoeff[0];
    var p = 1;
    for (var k = 1; k < n - ell; k++) {
        p = p * r;
        result += p * laguerreCoeff[k];
    }
    result *= Math.pow(r, ell) * Math.exp(-r / n);
    return result;
}

// ---- verbatim port of initLookupTableRadial(n, ell), parameterized on table size ----

function initLookupTableRadial(n, ell, nTableRadial) {
    var maxR = 6 * n * n;
    var deltaR = maxR / (nTableRadial - 1);
    var table = Array(nTableRadial);
    for (var i = 0; i < nTableRadial; i++) {
        var r = i * deltaR;
        table[i] = hydrogenRadialFunction(r, n, ell);
    }
    return { table: table, maxR: maxR };
}

// ---- verbatim port of getValueFromLookupTable(x, table) ----

function getValueFromLookupTable(x, table) {
    var n = table.length;
    var iFloat = x * (n - 1);
    iFloat = Math.max(0, iFloat);
    var i = Math.floor(iFloat);
    var eta = iFloat - i;
    if (i < n - 1)
        return table[i] * (1 - eta) + table[i + 1] * eta;
    else
        return table[n - 1];
}

// ---- not from the original file: see header comment ----

function psiReal(r, theta, phi, n, ell, m) {
    var R = hydrogenRadialFunction(r, n, ell);
    var P = computePLM(theta, ell, m);
    var azimuthal = (m >= 0) ? Math.cos(m * phi) : Math.sin(-m * phi);
    return R * P * azimuthal;
}

// ---- point cloud sampling (M2) -- not from the original file ----
//
// Samples r, theta, phi from three INDEPENDENT precomputed inverse-CDF
// (quantile) tables rather than rejection sampling. This is exact, not an
// approximation: the target density factors as
//   |psi|^2 * r^2 * sin(theta) = [r*R(r)]^2 * [P_l^m(theta)^2*sin(theta)] * azimuthal(phi)^2
// i.e. a product of three single-variable functions, so sampling each
// marginal independently and combining the results reproduces the joint
// density exactly (the same factorization a prior separable-rejection
// version -- preserved in git history -- exploited). Each marginal's
// inverse CDF is built once per orbital with a single monotonic forward
// sweep (no per-point search), so sampling a point costs exactly three
// table lookups (interpolated, O(1)) plus the trig to convert to Cartesian
// -- no rejection loop anywhere, so no variance in per-point cost. This
// refines an earlier per-axis *rejection* version (also preserved in git
// history) the same way stef1949/Electron-Orbital-Simulator's GPU sampler
// does it: precompute the inverse function itself, not just the CDF, so
// there's no search at sample time either.
//
// Mirrored bit-for-bit in src/pointcloud.h/.cpp and micropython/pointcloud.py
// so that, given the same seed, all three ports produce the *same* sequence
// of points (see tools/orbitals_host/README.md).

// Portable xorshift32 PRNG (Marsaglia's (13,17,5) triple). Not
// cryptographically secure -- chosen only because it is trivial to port
// bit-for-bit to C++ and MicroPython, which is what lets the point clouds
// produced by all three ports be compared for exact agreement rather than
// just statistically.
function XorShift32(seed) {
    this.state = (seed >>> 0) !== 0 ? (seed >>> 0) : 1;
}

XorShift32.prototype.next = function () {
    var x = this.state;
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    this.state = x >>> 0;
    return this.state;
};

XorShift32.prototype.uniform01 = function () {
    return this.next() / 4294967296; // 2^32
};

// Given non-negative sample weights of a density over [0, domainMax] taken
// at evenly spaced points, build the inverse CDF: result[k] is the x-value
// at quantile k/(weight.length-1). Both the forward cumulative sum and the
// inverse lookup below are single monotonic sweeps (the CDF is
// non-decreasing, and the target quantile is non-decreasing in k), so this
// is O(weight.length) total -- no per-point search survives into sampling
// either, since the result is later read directly via getValueFromLookupTable().
function buildInverseCdf(weight, domainMax) {
    var count = weight.length;
    var delta = domainMax / (count - 1);

    var cdf = new Array(count);
    var cumulative = 0;
    for (var i = 0; i < count; i++) {
        cumulative += weight[i];
        cdf[i] = cumulative;
    }
    var total = cdf[count - 1];
    if (total <= 0) total = 1; // degenerate guard; shouldn't occur for valid quantum numbers
    for (var ii = 0; ii < count; ii++) cdf[ii] /= total;

    var invTable = new Array(count);
    var j = 0;
    for (var k = 0; k < count; k++) {
        var u = k / (count - 1);
        while (j < count - 1 && cdf[j] < u) j++;
        var j0 = j > 0 ? j - 1 : 0;
        var j1 = j;
        var c0 = cdf[j0];
        var c1 = cdf[j1];
        var t = (c1 > c0) ? (u - c0) / (c1 - c0) : 0;
        invTable[k] = (j0 + t * (j1 - j0)) * delta;
    }
    return invTable;
}

// Precompute an "OrbitalSampler" for (n, ell, m): builds the three
// inverse-CDF tables above from initLookupTableRadial()'s r*R(r),
// initLookupTable()'s P_l^m(theta), and the azimuthal factor
// cos(m*phi)/sin(|m|*phi) (evaluated directly, no separate table dependency).
function initOrbitalSampler(n, ell, m) {
    initLegendreCoeffs(ell, m);
    initLaguerreCoeffs(n, ell);

    var radial = initLookupTableRadial(n, ell, 1001);
    var deltaR = radial.maxR / (radial.table.length - 1);
    var rWeight = new Array(radial.table.length);
    for (var i = 0; i < radial.table.length; i++) {
        var rr = radial.table[i] * (i * deltaR);
        rWeight[i] = rr * rr;
    }
    var invRTable = buildInverseCdf(rWeight, radial.maxR);

    var legendreTable = initLookupTable(ell, m, 1001);
    var deltaTheta = Math.PI / (legendreTable.length - 1);
    var thetaWeight = new Array(legendreTable.length);
    for (var j = 0; j < legendreTable.length; j++) {
        var theta = j * deltaTheta;
        thetaWeight[j] = legendreTable[j] * legendreTable[j] * Math.sin(theta);
    }
    var invThetaTable = buildInverseCdf(thetaWeight, Math.PI);

    var phiSize = 1001;
    var twoPi = 2 * Math.PI;
    var deltaPhi = twoPi / (phiSize - 1);
    var phiWeight = new Array(phiSize);
    for (var p = 0; p < phiSize; p++) {
        var phi = p * deltaPhi;
        var azimuthal = (m >= 0) ? Math.cos(m * phi) : Math.sin(-m * phi);
        phiWeight[p] = azimuthal * azimuthal; // m==0 -> constant 1, uniform phi
    }
    var invPhiTable = buildInverseCdf(phiWeight, twoPi);

    return {
        n: n,
        ell: ell,
        m: m,
        maxR: radial.maxR,
        invRTable: invRTable,
        invThetaTable: invThetaTable,
        invPhiTable: invPhiTable,
    };
}

// Draw one point from the (r, theta, phi) probability density
// |psi_{n,l,m}|^2 * r^2 * sin(theta) via inverse-CDF (quantile) sampling:
// one uniform draw per axis, mapped through that axis's precomputed inverse
// table via linear interpolation (getValueFromLookupTable()). Always
// exactly 3 RNG draws per point, in the fixed order (r, theta, phi) --
// mirrored exactly in the C++ and MicroPython ports so identical seeds
// produce identical points.
function sampleOrbitalPoint(sampler, rng) {
    var r = getValueFromLookupTable(rng.uniform01(), sampler.invRTable);
    var theta = getValueFromLookupTable(rng.uniform01(), sampler.invThetaTable);
    var phi = getValueFromLookupTable(rng.uniform01(), sampler.invPhiTable);

    var sinTheta = Math.sin(theta);
    return {
        x: r * sinTheta * Math.cos(phi),
        y: r * sinTheta * Math.sin(phi),
        z: r * Math.cos(theta),
    };
}

module.exports = {
    initLegendreCoeffs,
    computePLM,
    initLookupTable,
    initLaguerreCoeffs,
    hydrogenRadialFunction,
    initLookupTableRadial,
    getValueFromLookupTable,
    psiReal,
    legendreCoeff,
    laguerreCoeff,
    XorShift32,
    initOrbitalSampler,
    sampleOrbitalPoint,
};
