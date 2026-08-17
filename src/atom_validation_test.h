// Validation-data-dump build: prints electron-configuration/Z_eff/point-sample data for a
// curated element set as tagged CSV log lines, for comparison against
// tools/orbitals_host/gen_atom_reference.py's host reference (run under the real
// MicroPython unix port -- see that script's docstring and
// tools/orbitals_host/compare_atom.py). Skips LCD init entirely; capture with e.g.
// `pio device monitor > capture.log`, then run compare_atom.py on it.
#pragma once

void runAtomValidationTest();
