#include "overlay.h"

#include <cstdio>

#include "display.h"
#include "font.h"

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

static constexpr int kScaleBarMarginX = 8;
static constexpr int kScaleBarMarginY = 8;
static constexpr orb_real_t kScaleBarMaxPx = orb_real_t(90);
static constexpr int kScaleBarTickPx = 4;

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

    for (int x = x0; x <= x1; x++)
        if (x >= 0 && x < Display::kDisplayWidth)
            frameBuf[y * Display::kDisplayWidth + x] = barColor;
    for (int ty = y - kScaleBarTickPx; ty <= y + kScaleBarTickPx; ty++)
    {
        if (ty < 0 || ty >= Display::kDisplayHeight)
            continue;
        if (x0 >= 0 && x0 < Display::kDisplayWidth)
            frameBuf[ty * Display::kDisplayWidth + x0] = barColor;
        if (x1 >= 0 && x1 < Display::kDisplayWidth)
            frameBuf[ty * Display::kDisplayWidth + x1] = barColor;
    }

    char text[32];
    std::snprintf(text, sizeof(text), "%s %s", len.label, unitLabel);
    drawText(frameBuf, x0, y - kScaleBarTickPx - 12, text, textColor);
}
