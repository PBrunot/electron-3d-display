#!/usr/bin/env node
// Generate JS-reference CSV artifacts for the hydrogen-orbital math cross-check.
// Usage: node gen_js_reference.js <test_cases.csv> <out_dir>
'use strict';

const fs = require('fs');
const path = require('path');
const ref = require('./js_reference.js');

const TABLE_SIZE = 1001;

// Sample grid for psi_samples.csv -- MUST match the constants in
// gen_c_reference.cpp exactly (r as a fraction of maxR, theta/phi in radians).
const R_FRACS = [0.0, 0.02, 0.05, 0.1, 0.2, 0.35, 0.55, 0.8];
const THETA_VALUES = [0, 1, 2, 3, 4, 5, 6].map((i) => (Math.PI * i) / 6);
const PHI_VALUES = [0, 0.25, 0.5, 1.0, 1.5].map((f) => Math.PI * f);

function parseTestCases(csvPath) {
    const lines = fs.readFileSync(csvPath, 'utf8').split('\n');
    const cases = [];
    for (const line of lines) {
        const trimmed = line.trim();
        if (trimmed === '' || trimmed.startsWith('#')) continue;
        const [n, l, m] = trimmed.split(',').map((s) => parseInt(s, 10));
        cases.push({ n, l, m });
    }
    return cases;
}

function writeCsv(filePath, header, rows) {
    const lines = [header.join(','), ...rows.map((r) => r.join(','))];
    fs.writeFileSync(filePath, lines.join('\n') + '\n');
}

function prefix(n, l, m) {
    return `${n}_${l}_${m}`;
}

function main() {
    const [, , testCasesPath, outDir] = process.argv;
    if (!testCasesPath || !outDir) {
        console.error('Usage: node gen_js_reference.js <test_cases.csv> <out_dir>');
        process.exit(1);
    }
    fs.mkdirSync(outDir, { recursive: true });

    const cases = parseTestCases(testCasesPath);
    for (const { n, l, m } of cases) {
        const p = prefix(n, l, m);

        // initLegendreCoeffs() deliberately does not clear legendreCoeff between
        // calls (only same-parity indices are ever read back by computePLM, so
        // stale opposite-parity entries from a previous (ell,m) are harmless) --
        // but that leftover state makes an apples-to-apples coeff dump vs. the
        // C++ port (which does start from zero) misleading. Reset first so the
        // comparison reflects the coefficients this call actually produced.
        ref.legendreCoeff.fill(0);
        ref.initLegendreCoeffs(l, m);
        const legendreCoeffSnapshot = ref.legendreCoeff.slice(0, l + 1);

        ref.initLaguerreCoeffs(n, l);
        const laguerreCoeffSnapshot = ref.laguerreCoeff.slice(0, Math.max(1, n - l));

        // coeffs.csv
        const coeffRows = [];
        legendreCoeffSnapshot.forEach((v, i) => coeffRows.push(['legendre', i, v]));
        laguerreCoeffSnapshot.forEach((v, i) => coeffRows.push(['laguerre', i, v]));
        writeCsv(path.join(outDir, `${p}_coeffs.csv`), ['kind', 'index', 'value'], coeffRows);

        // legendre_table.csv (re-init: computePLM reads the module-level
        // legendreCoeff, which initLaguerreCoeffs above did not touch, but be
        // explicit/order-independent like the C++ side is)
        ref.initLegendreCoeffs(l, m);
        const legendreTable = ref.initLookupTable(l, m, TABLE_SIZE);
        const legendreRows = legendreTable.map((v, i) => [i, (Math.PI * i) / (TABLE_SIZE - 1), v]);
        writeCsv(path.join(outDir, `${p}_legendre_table.csv`), ['i', 'theta', 'value'], legendreRows);

        // radial_table.csv
        const { table: radialTable, maxR } = ref.initLookupTableRadial(n, l, TABLE_SIZE);
        const deltaR = maxR / (TABLE_SIZE - 1);
        const radialRows = radialTable.map((v, i) => [i, i * deltaR, v]);
        fs.writeFileSync(
            path.join(outDir, `${p}_radial_table.csv`),
            `# maxR=${maxR}\ni,r,value\n` + radialRows.map((r) => r.join(',')).join('\n') + '\n'
        );

        // psi_samples.csv
        ref.initLegendreCoeffs(l, m); // ensure legendreCoeff matches (l, m) before psiReal calls computePLM
        ref.initLaguerreCoeffs(n, l); // ensure laguerreCoeff matches (n, l) before psiReal calls hydrogenRadialFunction
        const psiRows = [];
        for (const rf of R_FRACS) {
            const r = rf * maxR;
            for (const theta of THETA_VALUES) {
                for (const phi of PHI_VALUES) {
                    const psi = ref.psiReal(r, theta, phi, n, l, m);
                    psiRows.push([rf, theta, phi, psi]);
                }
            }
        }
        writeCsv(path.join(outDir, `${p}_psi_samples.csv`), ['r_frac', 'theta', 'phi', 'psi'], psiRows);

        console.log(`JS: wrote artifacts for n=${n} l=${l} m=${m}`);
    }
}

main();
