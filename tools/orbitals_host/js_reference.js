// Ground-truth reference for hydrogen orbital math, used to cross-check the
// C++ port in src/orbitals.cpp.
//
// This is a near-verbatim extraction of the orbital-math functions from
// quantum-physics.js (c) 2020-2022 Manuel Joffre, www.quantum-physics.polytechnique.fr
// (see examples/js-calculations/quantum-physics.js for the full original file,
// which also contains unrelated double-slit-experiment / FFT / 2D-plotting
// code not needed here). Function bodies below are copied unmodified from
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

// ---- rejection-sampled point cloud (M2) -- not from the original file ----
//
// Mirrored bit-for-bit in src/pointcloud.h/.cpp and micropython/pointcloud.py
// so that, given the same seed, all three ports produce the *same* sequence
// of accepted points (see tools/orbitals_host/README.md).

// Multiplies the tabulated peak-magnitude bound by a small margin, since the
// true continuous peak of a smooth function can fall between two of the
// TABLE_SIZE tabulated points. Not a rigorous guarantee, but ample for a
// point-cloud visualization. Mirrored exactly (same literal) in
// src/pointcloud.cpp and micropython/pointcloud.py.
var DENSITY_BOUND_MARGIN = 1.15;

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

// Precompute an "OrbitalSampler" for (n, ell, m): sets the module-level
// legendreCoeff/laguerreCoeff (via initLegendreCoeffs/initLaguerreCoeffs, as
// psiReal() reads them implicitly) plus a valid upper bound on the target
// density, obtained from the peak magnitudes of initLookupTableRadial()'s
// r*R(r) and initLookupTable()'s P_l^m(theta) (each padded by
// DENSITY_BOUND_MARGIN).
function initOrbitalSampler(n, ell, m) {
    initLegendreCoeffs(ell, m);
    initLaguerreCoeffs(n, ell);

    var radial = initLookupTableRadial(n, ell, 1001);
    var deltaR = radial.maxR / (radial.table.length - 1);
    var maxRr = 0;
    for (var i = 0; i < radial.table.length; i++) {
        var rr = Math.abs(radial.table[i] * (i * deltaR));
        if (rr > maxRr) maxRr = rr;
    }

    var legendreTable = initLookupTable(ell, m, 1001);
    var maxP = 0;
    for (var j = 0; j < legendreTable.length; j++) {
        var p = Math.abs(legendreTable[j]);
        if (p > maxP) maxP = p;
    }

    return {
        n: n,
        ell: ell,
        m: m,
        maxR: radial.maxR,
        densityBound: maxRr * maxRr * maxP * maxP * DENSITY_BOUND_MARGIN,
    };
}

// Draw one point from the (r, theta, phi) probability density
// |psi_{n,l,m}|^2 * r^2 * sin(theta) via rejection sampling. Fixed draw
// order (r, theta, phi, u) per attempt -- mirrored exactly in the C++ and
// MicroPython ports so identical seeds produce identical accept/reject
// sequences and identical accepted points.
function sampleOrbitalPoint(sampler, rng, maxAttempts) {
    maxAttempts = maxAttempts || 100000;
    for (var attempt = 0; attempt < maxAttempts; attempt++) {
        var r = rng.uniform01() * sampler.maxR;
        var theta = rng.uniform01() * Math.PI;
        var phi = rng.uniform01() * (2 * Math.PI);
        var u = rng.uniform01() * sampler.densityBound;

        var psi = psiReal(r, theta, phi, sampler.n, sampler.ell, sampler.m);
        var density = (psi * r) * (psi * r) * Math.sin(theta);

        if (u <= density) {
            var sinTheta = Math.sin(theta);
            return {
                x: r * sinTheta * Math.cos(phi),
                y: r * sinTheta * Math.sin(phi),
                z: r * Math.cos(theta),
            };
        }
    }
    return { x: 0, y: 0, z: 0 };
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
