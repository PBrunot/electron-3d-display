// Standalone single-hardcoded-atom test build: M1/M2-era render path (uniform white
// points, single kAtomicNumber, no shell coloring) kept around as an alternate entry
// point for comparing against the orbital viewer or debugging atom_cloud.h in isolation.
// Superseded for real parity by atom_view.h/.cpp (M4, not started yet) -- do not add
// shell-coloring/calibration work here, extend M4's file instead.
#pragma once
#include "render/display.h"

void runAtomViewTest(Display &display);
