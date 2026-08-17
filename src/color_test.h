// Full-screen solid-color diagnostic: cycles through known reference colors via
// Display::packColor565(), logging the intended RGB, the packed RGB565 value, and
// round-trip results from unpackColor565()/blendColor565()/fadeColor565() over serial --
// lets a color problem be narrowed down to "the physical panel colors don't match
// CLAUDE.md's documented mapping" (visual) vs. "the blend/fade math is wrong"
// (log-only, no visual judgment needed) before touching any real render path.
#pragma once

#include "display.h"

/** Cycles kTestColors forever, one full-screen fill every ~2s, logging each one's pack/
 * unpack/blend/fade values. Never returns. */
void runColorTest(Display& display);
