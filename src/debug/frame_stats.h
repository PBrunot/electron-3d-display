// Per-frame timing/FPS bookkeeping shared by atom_view.cpp and orbital_view.cpp's
// steady-state render loops.
#pragma once

#include <cstdint>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config/visual_constants.h" // kFpsUpdateInterval

/// Fixed-window moving average over the last kWindow samples (ring buffer, no heap
/// allocation -- cheap enough to update every frame on-device).
template <int kWindow>
class MovingAverage
{
public:
    void add(double sample)
    {
        sum_ += sample - buf_[pos_];
        buf_[pos_] = sample;
        pos_ = (pos_ + 1) % kWindow;
        if (count_ < kWindow)
            count_++;
    }

    double average() const { return count_ > 0 ? sum_ / count_ : 0.0; }

private:
    double buf_[kWindow] = {};
    double sum_ = 0.0;
    int pos_ = 0;
    int count_ = 0;
};

/// Width of the render/prepare moving averages below, in frames.
inline constexpr int kFrameStatsWindow = 100;

/**
 * @brief FPS + per-phase timing tracker for a view's steady-state loop.
 *
 * "Prepare" is time blocked in Display::waitForFlushDone() (the previous frame's SPI DMA
 * finishing before frameBuf can be overwritten again); "render" is time spent drawing the
 * frame plus kicking off presentFrame()'s DMA. Both are tracked as a moving average over the
 * last kFrameStatsWindow frames, so a periodic log line reflects recent steady-state cost
 * instead of one outlier frame. FPS itself stays a plain frames/elapsed count over
 * kFpsUpdateInterval frames -- unrelated window, see maybeLog().
 */
struct FrameStats
{
    int frameCount = 0;
    int64_t windowStartUs = 0;
    int64_t lastLoadMs = 0; ///< Point-cloud build time of the last load()/switch (see AtomPresetState/OrbitalPresetState::loadMs).
    MovingAverage<kFrameStatsWindow> prepareMs;
    MovingAverage<kFrameStatsWindow> renderMs;

    /// Restarts the FPS window -- call whenever the loop spends real wall-clock time that
    /// shouldn't count against steady-state FPS (element/preset switches, dissection,
    /// zoom excursions -- anything that already renders its own frames via a fly-over).
    void reset()
    {
        frameCount = 0;
        windowStartUs = esp_timer_get_time();
    }

    /// Feeds one steady-state frame's prepare/render durations (ms) into the moving
    /// averages and the FPS frame count.
    void recordFrame(double prepareMsSample, double renderMsSample)
    {
        prepareMs.add(prepareMsSample);
        renderMs.add(renderMsSample);
        frameCount++;
    }

    /// Logs FPS, the render/prepare moving averages, the last load time, and free internal
    /// RAM every kFpsUpdateInterval frames, then resets the FPS window. No-op otherwise.
    void maybeLog(const char *tag)
    {
        if (frameCount < kFpsUpdateInterval)
            return;
        int64_t nowUs = esp_timer_get_time();
        double elapsedS = double(nowUs - windowStartUs) / 1e6;
        double fps = elapsedS > 0 ? double(frameCount) / elapsedS : 0.0;
        ESP_LOGI(tag,
                 "FPS: %.1f, render_ms(avg%d): %.2f, prepare_ms(avg%d): %.2f, last_load_ms: %lld, iram_free: %u",
                 fps, kFrameStatsWindow, renderMs.average(), kFrameStatsWindow, prepareMs.average(), lastLoadMs,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        windowStartUs = nowUs;
        frameCount = 0;
    }
};
