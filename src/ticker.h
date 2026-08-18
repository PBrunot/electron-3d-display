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

constexpr int kTickerDefaultPxPerFrame = 5;

/**
 * Scroll `text` once, right-to-left, from fully off the right edge to fully off the left
 * edge, at `scale`x font-pixel size (see font.h's drawTextScaled()) -- blocking until the
 * pass completes. Clears to black and redraws every frame (no other overlay content); a
 * caller wanting a title/proton marker etc. visible during the scroll composes that
 * itself instead of calling this. Paced only by presentFrame()/waitForFlushDone() (no
 * artificial delay), same as camera.h's flyOver() -- the SPI frame transfer is already the
 * dominant per-frame cost, so an extra vTaskDelay would only slow down an effect that's
 * supposed to read as fast.
 */
void scrollTextOnce(Display &display, const char *text, const Font &font, int scale, uint16_t color, int y,
                    int pxPerFrame = kTickerDefaultPxPerFrame);

/**
 * Like scrollTextOnce(), but slides in from the right, PAUSES for `holdMs` once `text` is
 * centered on screen (real wall-clock time, vTaskDelay -- independent of render speed),
 * then continues sliding out past the left edge -- "slide in, pause, slide out" instead of
 * one continuous pass, giving the eye a moment to catch the text before it's gone. If
 * `text` is wider than the screen at `scale`, "centered" still means its midpoint aligns
 * with the screen's midpoint (both ends run off-screen during the pause) -- expected for a
 * long sentence at a size chosen for legibility-in-motion, not to fit statically.
 */
void scrollTextPauseOnce(Display &display, const char *text, const Font &font, int scale, uint16_t color, int y,
                         uint32_t holdMs, int pxPerFrame = kTickerDefaultPxPerFrame);
