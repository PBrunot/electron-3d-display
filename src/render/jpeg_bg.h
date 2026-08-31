// Shared JPEG-background decoder, used by splash_bitmap.cpp and orbitals_bitmap.cpp.
#pragma once

class Display;

/**
 * @brief Decode `path` (a JPEG on the "storage" SPIFFS partition) and blit it centered on the
 *        display, opaque. `width`/`height` must match the JPEG's own size (mismatch is a no-op,
 *        logged). Letterbox/pillarbox bars on a display bigger than the image are filled with
 *        the image's own sampled background color.
 *
 * Decodes straight into the frame buffer (no whole-image intermediate). Re-decodes every call
 * (~60-90ms for 240x240) -- call only when the background needs to (re)appear, not per-frame.
 * No-op (logged) on mount/open/decode failure.
 */
void drawJpegBackground(Display &display, const char *path, int width, int height);
