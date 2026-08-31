#include "render/orbitals_bitmap.h"

#include "render/jpeg_bg.h"

void drawOrbitalsBackdrop(Display &display)
{
    drawJpegBackground(display, "/storage/orbitals.jpg", kOrbitalsBitmapWidth, kOrbitalsBitmapHeight);
}
