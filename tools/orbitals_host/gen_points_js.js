#!/usr/bin/env node
// Generate JS-reference point-cloud CSV artifacts for the hydrogen-orbital
// rejection sampler. Usage: node gen_points_js.js <test_cases.csv> <out_dir>
'use strict';

const fs = require('fs');
const path = require('path');
const ref = require('./js_reference.js');

// MUST match the constants in gen_points_c.cpp and gen_points_mpy.py exactly.
const SEED = 12345;
const POINTS_PER_CASE = 100;

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

function prefix(n, l, m) {
    return `${n}_${l}_${m}`;
}

function main() {
    const [, , testCasesPath, outDir] = process.argv;
    if (!testCasesPath || !outDir) {
        console.error('Usage: node gen_points_js.js <test_cases.csv> <out_dir>');
        process.exit(1);
    }
    fs.mkdirSync(outDir, { recursive: true });

    for (const { n, l, m } of parseTestCases(testCasesPath)) {
        const p = prefix(n, l, m);
        const sampler = ref.initOrbitalSampler(n, l, m);
        const rng = new ref.XorShift32(SEED);

        const lines = ['index,x,y,z'];
        for (let i = 0; i < POINTS_PER_CASE; i++) {
            const pt = ref.sampleOrbitalPoint(sampler, rng);
            lines.push(`${i},${pt.x},${pt.y},${pt.z}`);
        }
        fs.writeFileSync(path.join(outDir, `${p}_points.csv`), lines.join('\n') + '\n');

        console.log(`JS: wrote ${POINTS_PER_CASE} points for n=${n} l=${l} m=${m}`);
    }
}

main();
