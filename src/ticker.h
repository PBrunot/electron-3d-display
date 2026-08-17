// Right-to-left scrolling banner text -- used as a fast, fun stinger between states (a new
// element's Italian name before atom_view.cpp switches to it, "Configurazione elettronica
// di <simbolo>" before its dissection sequence starts). Not a general text-effects module:
// just this one shape (full-width, full-screen-height black background, one pass, blocks
// until done), built on font.h's drawTextScaled()/textWidthScaled() since this project
// bakes no font size big enough on its own for a "big letters" banner.
#pragma once

#include <cstdint>

#include "display.h"
#include "font.h"

// Chosen for "fast and fun" (explicit feedback, 2026-08-17), not for readability at a
// leisurely pace -- see scrollTextOnce()'s docstring.
constexpr int kTickerDefaultPxPerFrame = 14;

/**
 * Scroll `text` once, right-to-left, from fully off the right edge to fully off the left
 * edge, at `scale`x font-pixel size (see font.h's drawTextScaled()) -- blocking until the
 * pass completes. Clears to black and redraws every frame (no other overlay content); a
 * caller wanting a title/proton marker etc. visible during the scroll composes that
 * itself instead of calling this. Paced only by presentFrame()/waitForFlushDone() (no
 * artificial delay), same as camera.h's flyOver() -- the SPI frame transfer is already the
 * dominant per-frame cost (CLAUDE.md section 5), so an extra vTaskDelay would only slow
 * down an effect that's supposed to read as fast.
 */
void scrollTextOnce(Display &display, const char *text, const Font &font, int scale, uint16_t color, int y,
                     int pxPerFrame = kTickerDefaultPxPerFrame);
