// NOT a generated file (unlike before) -- see splash_bitmap.cpp for why. The source image
// itself is still img/atomic_cube.jpg (mirrored at data/atomic_cube.jpg, deployed to the
// "storage" SPIFFS partition via the existing uploadfs step -- see util/storage_mount.h),
// unchanged, decoded on-device now instead of being pre-decoded offline by
// tools/splash_gen/render_splash.py (now unused -- kept in tools/ only as a record of the old
// approach).
#pragma once

class Display;

inline constexpr int kSplashBitmapWidth = 240;
inline constexpr int kSplashBitmapHeight = 240;

/**
 * @brief Decode data/atomic_cube.jpg and blit it centered on the display, opaque (no blending)
 *        -- caller presents the frame afterward. Clipped against Display::kDisplayWidth/Height
 *        like every other draw function in this project; on a panel whose size doesn't match
 *        kSplashBitmapWidth/Height (e.g. the CYD's 240x320 vs. the image's 240x240) the
 *        letterbox/pillarbox bars left over are filled with the image's own background color
 *        (sampled from its top-left pixel), not left holding whatever the screen had before.
 *
 * Decodes straight into the Display's own frame buffer via writePx() -- no intermediate RGB565
 * buffer for the whole image (that would be 240*240*2 = 115200 bytes; an early version of this
 * cached exactly that, persistently, and overflowed the CYD's internal DRAM at link time, see
 * splash_bitmap.cpp's header comment). Every call re-reads and re-decodes the JPEG (~60-90ms,
 * logged) -- this is only called when the background actually needs to appear (boot splash,
 * and ux/chooser.cpp's menu redrawing it once per re-entry, not per frame -- see that file),
 * never in a per-frame hot path, so re-decoding each time costs nothing that matters.
 *
 * A no-op (logged) if the storage partition won't mount, the file is missing, or the JPEG
 * fails to decode -- callers just get no background for that draw, not a crash.
 */
void drawSplashScreen(Display &display);
