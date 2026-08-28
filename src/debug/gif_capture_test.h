/**
 * @file gif_capture_test.h
 * @brief Dev toggle (see main.cpp's GIF_CAPTURE_TEST) that renders a fixed rotating sequence
 *        of one atom to the live display AND saves each frame as a numbered PNG on the
 *        "storage" SPIFFS partition (gif_000.png, gif_001.png, ...), for pulling with
 *        pc/pull_screenshots.py --all and assembling into an actual GIF on the PC side.
 *
 * Exists to let config/visual_constants.h's kElectronAlphaQ8/kPersistenceKeepQ8 (per-point
 * alpha blend, per-frame persistence fade) be judged visually -- run once as committed, once
 * with those two constants set to their "off" values (256 / 0), and diff the two pulled
 * sequences -- without building a separate host renderer.
 */
#pragma once

#include "render/display.h"

/// Renders and saves kGifFrameCount frames of the fixed benchmark element (Fe, Z=26 -- same
/// "complex enough to be representative" pick as debug/benchmark_test.cpp and
/// debug/screenshot_batch.cpp's Fe dissection) tumbling through renderAtomFrame()'s normal
/// camera step. Never returns -- caller loops afterward like runBenchmarkTest()'s caller does.
void runGifCaptureTest(Display &display);
