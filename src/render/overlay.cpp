#include "render/overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "render/display.h"
#include "render/font.h"

struct ScaleBarLength
{
    orb_real_t value;
    const char *label;
};

// Same "nice round length" ladder as cloud_common.SCALE_BAR_CANDIDATES (1/2/5 x a power
// of ten) with precomputed display strings, so picking one never needs runtime
// float-to-string formatting beyond what snprintf already gives us for the unit suffix.
static constexpr ScaleBarLength kScaleBarCandidates[] = {
    {orb_real_t(0.001), "0.001"},
    {orb_real_t(0.002), "0.002"},
    {orb_real_t(0.005), "0.005"},
    {orb_real_t(0.01), "0.01"},
    {orb_real_t(0.02), "0.02"},
    {orb_real_t(0.05), "0.05"},
    {orb_real_t(0.1), "0.1"},
    {orb_real_t(0.2), "0.2"},
    {orb_real_t(0.5), "0.5"},
    {orb_real_t(1), "1"},
    {orb_real_t(2), "2"},
    {orb_real_t(5), "5"},
    {orb_real_t(10), "10"},
    {orb_real_t(20), "20"},
    {orb_real_t(50), "50"},
    {orb_real_t(100), "100"},
    {orb_real_t(200), "200"},
    {orb_real_t(500), "500"},
    {orb_real_t(1000), "1000"},
};
static constexpr int kScaleBarCandidateCount = sizeof(kScaleBarCandidates) / sizeof(kScaleBarCandidates[0]);

static constexpr int kScaleBarMarginX = 16;
static constexpr int kScaleBarMarginY = 16;
static constexpr orb_real_t kScaleBarMaxPx = orb_real_t(180); ///< Longest the bar is allowed to render, px.
static constexpr int kScaleBarTickPx = 8;                     ///< End-tick half-height, px.
static constexpr int kScaleBarLabelGapPx = 4;                 ///< Gap between label bottom and the tick top.
static constexpr int kScaleBarLineThicknessPx = 2;            ///< Bar/tick stroke width, px.

/**
 * Largest candidate from kScaleBarCandidates whose on-screen length (value *
 * pixelsPerUnit) still fits under maxBarPx -- the most precise round number the bar can
 * show without overflowing. Falls back to the smallest candidate if even that one would
 * be too long (only at extreme zoom-in). Port of cloud_common.pick_scale_bar_length().
 */
static ScaleBarLength pickScaleBarLength(orb_real_t pixelsPerUnit, orb_real_t maxBarPx)
{
    ScaleBarLength best = kScaleBarCandidates[0];
    for (int i = 0; i < kScaleBarCandidateCount; i++)
    {
        if (kScaleBarCandidates[i].value * pixelsPerUnit <= maxBarPx)
            best = kScaleBarCandidates[i];
        else
            break;
    }
    return best;
}

void drawScaleBar(uint16_t *frameBuf, orb_real_t pixelsPerUnit, const char *unitLabel, uint16_t barColor,
                  uint16_t textColor)
{
    if (pixelsPerUnit <= orb_real_t(0))
        return;
    ScaleBarLength len = pickScaleBarLength(pixelsPerUnit, kScaleBarMaxPx);
    int barPx = int(len.value * pixelsPerUnit);
    if (barPx < 1)
        barPx = 1;

    int x0 = kScaleBarMarginX;
    int y = Display::kDisplayHeight - kScaleBarMarginY;
    int x1 = x0 + barPx;

    // Row-major (not column-major) so each row's span is one contiguous fill_n instead of
    // kDisplayWidth-strided single-pixel writes.
    int xLo = x0 > 0 ? x0 : 0;
    int xHi = x1 < Display::kDisplayWidth - 1 ? x1 : Display::kDisplayWidth - 1;
    if (xHi >= xLo)
        for (int ly = y; ly < y + kScaleBarLineThicknessPx && ly < Display::kDisplayHeight; ly++)
            std::fill_n(frameBuf + ly * Display::kDisplayWidth + xLo, xHi - xLo + 1, barColor);
    for (int ty = y - kScaleBarTickPx; ty <= y + kScaleBarTickPx; ty++)
    {
        if (ty < 0 || ty >= Display::kDisplayHeight)
            continue;
        for (int lx = 0; lx < kScaleBarLineThicknessPx; lx++)
        {
            if (x0 + lx >= 0 && x0 + lx < Display::kDisplayWidth)
                frameBuf[ty * Display::kDisplayWidth + x0 + lx] = barColor;
            if (x1 + lx >= 0 && x1 + lx < Display::kDisplayWidth)
                frameBuf[ty * Display::kDisplayWidth + x1 + lx] = barColor;
        }
    }

    char text[32];
    // kScaleBarCandidates tops out at 1000 (pm) -- past that round number the bar reads
    // clearer in nm than as a 4-digit pm count. Geometry above (barPx) still comes from
    // len.value in its original pm-based pixelsPerUnit, only the label text changes here.
    if (std::strcmp(unitLabel, "pm") == 0 && len.value >= orb_real_t(1000))
        std::snprintf(text, sizeof(text), "%d nm", int(len.value / orb_real_t(1000)));
    else
        std::snprintf(text, sizeof(text), "%s %s", len.label, unitLabel);
    // kFontLarge at its own true size, not kFontSmall integer-upscaled -- the doubled
    // pixels read as blocky on-device (same issue kFontHuge exists to avoid for the
    // element-symbol title, see font.h).
    drawText(frameBuf, x0, y - kScaleBarTickPx - kScaleBarLabelGapPx - kFontLarge.height, text, textColor, kFontLarge);
}

/// Set (x, y) if it lands on-screen.
static void plotCirclePixel(uint16_t *frameBuf, int x, int y, uint16_t color)
{
    if (x >= 0 && x < Display::kDisplayWidth && y >= 0 && y < Display::kDisplayHeight)
        frameBuf[y * Display::kDisplayWidth + x] = color;
}

/// Set (cx+dx, cy+dy) and its 8-way mirror around (cx, cy), each clipped individually.
static void plotCircleOctants(uint16_t *frameBuf, int cx, int cy, int dx, int dy, uint16_t color)
{
    plotCirclePixel(frameBuf, cx + dx, cy + dy, color);
    plotCirclePixel(frameBuf, cx - dx, cy + dy, color);
    plotCirclePixel(frameBuf, cx + dx, cy - dy, color);
    plotCirclePixel(frameBuf, cx - dx, cy - dy, color);
    plotCirclePixel(frameBuf, cx + dy, cy + dx, color);
    plotCirclePixel(frameBuf, cx - dy, cy + dx, color);
    plotCirclePixel(frameBuf, cx + dy, cy - dx, color);
    plotCirclePixel(frameBuf, cx - dy, cy - dx, color);
}

void drawBoundingCircle(uint16_t *frameBuf, orb_real_t rRef, orb_real_t scale, uint16_t color)
{
    int r = int(rRef * scale + orb_real_t(0.5));
    if (r <= 0)
        return;
    int cx = Display::kDisplayWidth / 2;
    int cy = Display::kDisplayHeight / 2;

    // Midpoint circle algorithm: integer-only 8-way-symmetric outline, cheaper per frame than
    // the sin/cos parametric sweep drawScaleBar's caller-side breathing animation already
    // spends its float budget on.
    int x = 0, y = r;
    int d = 1 - r;
    plotCircleOctants(frameBuf, cx, cy, x, y, color);
    while (x < y)
    {
        x++;
        if (d < 0)
            d += 2 * x + 1;
        else
        {
            y--;
            d += 2 * (x - y) + 1;
        }
        plotCircleOctants(frameBuf, cx, cy, x, y, color);
    }
}
