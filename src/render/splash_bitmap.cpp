#include "render/splash_bitmap.h"

#include "render/jpeg_bg.h"

void drawSplashScreen(Display &display)
{
    drawJpegBackground(display, "/storage/atomic_cube.jpg", kSplashBitmapWidth, kSplashBitmapHeight);
}
