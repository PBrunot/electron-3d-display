// Standalone rendering-performance benchmark: builds a fixed atom's point cloud at several
// point-cloud sizes and times real-render-path (renderScene()) frames at each size, so
// performance can be compared across firmware revisions/hardware from a serial capture.
// Also emits a handful of deterministic physics-model numbers (electron configuration,
// Z_eff per subshell, outer-subshell reference radius/scale) as a cheap correctness
// fingerprint -- same seed + same Z always reproduces the same numbers, so a diff against a
// prior capture catches an accidental regression in the sampling/Z_eff math, not just a
// speed regression. Config/Z_eff for the fixed element (Fe, Z=26) are additionally directly
// comparable against tools/orbitals_host/gen_atom_reference.py's host reference (Fe is one
// of atom_validation_test.cpp's kValidationZs) if a stronger check is ever needed -- see
// main.cpp's BENCHMARK_TEST toggle to run this instead of the normal chooser.
#pragma once
#include "display.h"

void runBenchmarkTest(Display &display);
