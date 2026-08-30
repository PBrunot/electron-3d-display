// Minimal runtime menu: boots here, tilt-hold Up launches the hydrogen-orbital viewer,
// tilt-hold Down launches the multi-electron element viewer -- same direction pairing
// each viewer's own tilt-hold Up ("back to menu") returns to, and thematically consistent
// with Down also meaning "dissect down into the atom" once inside atom_view. Port of
// micropython/chooser.py's role (its ORBITALS_LABEL/ATOM_LABEL used the identical
// Nudge-Up/Nudge-Down pairing), simplified to a static two-line label instead of a
// tumbling backdrop preset -- not worth the extra point-cloud state for a menu screen.
#pragma once

#include "render/display.h"
#include "ux/tilt_gesture.h"

/**
 * Guided one-time direction calibration: prompts the user to tilt-and-hold each of
 * Right/Left/Up/Down in turn (the same physical gesture as normal use -- see
 * tilt_gesture.h's header comment), and calls TiltGestureDetector::setMapping() for each
 * one as it's confirmed via TiltGestureDetector::pollRaw(). Call once at boot, after
 * TiltGestureDetector::calibrate() (the planar baseline) and before relying on poll()
 * returning any direction -- runChooser() below already does this before its menu loop;
 * call directly only from an alternate entry point that bypasses runChooser() (e.g.
 * main.cpp's ATOM_VIEW direct-test branch).
 */
void calibrateDirections(Display &display, TiltGestureDetector &tilt);

/** Runs forever: shows the menu, launches a viewer on tilt-hold confirm, and shows the
 * menu again whenever that viewer returns (its own tilt-hold Up). Also auto-launches a
 * viewer -- coin-flipping orbitals vs. elements -- after 30s of no confirmed tilt-hold,
 * so the splash doesn't sit idle forever (see chooser.cpp's kChooserIdleJumpUs).
 * `tilt` is any GestureSource -- TiltGestureDetector or (CYD) touch_gesture.h's
 * TouchGestureDetector. */
void runChooser(Display &display, GestureSource &tilt);
