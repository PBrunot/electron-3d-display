#include "ticker.h"

#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

void scrollTextPauseOnce(Display &display, const char *text, const Font &font, int scale, uint16_t color, int y,
                          uint32_t holdMs, int pxPerFrame)
{
    int textPx = textWidthScaled(text, font, scale);
    int centerX = (Display::kDisplayWidth - textPx) / 2;

    auto renderAt = [&](int x)
    {
        display.waitForFlushDone();
        uint16_t *frameBuf = display.getFrameBuf();
        std::fill(frameBuf, frameBuf + Display::kDisplayWidth * Display::kDisplayHeight, Display::kColorBlack);
        drawTextScaled(frameBuf, x, y, text, color, font, scale);
        display.presentFrame();
    };

    for (int x = Display::kDisplayWidth; x > centerX; x -= pxPerFrame)
        renderAt(x);
    renderAt(centerX); // land exactly centered -- the loop above may step past it
    vTaskDelay(pdMS_TO_TICKS(holdMs));

    for (int x = centerX; x > -textPx; x -= pxPerFrame)
        renderAt(x);
}
