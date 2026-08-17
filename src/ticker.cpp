#include "ticker.h"

#include <algorithm>

void scrollTextOnce(Display &display, const char *text, const Font &font, int scale, uint16_t color, int y,
                     int pxPerFrame)
{
    int textPx = textWidthScaled(text, font, scale);
    int x = Display::kDisplayWidth;
    int endX = -textPx;

    while (x > endX)
    {
        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
        drawTextScaled(frameBuf, x, y, text, color, font, scale);
        display.presentFrame();

        x -= pxPerFrame;
    }
}
