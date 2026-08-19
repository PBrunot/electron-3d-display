#include "debug/benchmark_test.h"

#include <cstdio>

#include "physics/atom_cloud.h"
#include "render/camera.h"
#include "render/display.h"
#include "esp_attr.h" // EXT_RAM_BSS_ATTR
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "render/font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "render/overlay.h"
#include "physics/slater.h"

static const char *kBenchmarkTag = "benchmark";

// ============================================================================================
// Tunable constants
// ============================================================================================

/// Fixed element used at every sweep step, so the only thing varying between rows is point
/// count. Matches atom_view_test.cpp's choice (Fe: enough occupied subshells -- 1s..3d6 -- to
/// be a representative multi-shell cloud, not a trivial single-shell case) and is also one of
/// atom_validation_test.cpp's kValidationZs, so the CONFIG/ZEFF numbers logged below are
/// directly comparable against tools/orbitals_host/gen_atom_reference.py's host reference.
static constexpr int kBenchAtomicNumber = 26; // Fe

/// Point counts swept, ascending. kAtomNumPoints (atom_cloud.h) is the real production count
/// used by atom_view.cpp, so it's the ceiling here too -- also sizes the static point buffer
/// below, no reallocation between steps.
static constexpr int kBenchPointCounts[] = {500, 1000, 2000, 4000, 8000};
static constexpr int kBenchNumSteps = int(sizeof(kBenchPointCounts) / sizeof(kBenchPointCounts[0]));

static constexpr uint32_t kBenchSeed = kAtomCloudSeed; // same seed atom_view.cpp uses -- reproducible sweep

/// Frames rendered and timed per point-count step. Large enough to average out one-off
/// scheduling jitter without making the whole sweep take unreasonably long to capture.
static constexpr int kBenchFramesPerStep = 60;

// ============================================================================================

namespace
{
    struct StepStats
    {
        int points = 0;
        int64_t buildMs = 0;
        double avgRenderMs = 0, minRenderMs = 0, maxRenderMs = 0;
        double avgWaitMs = 0;
        double fps = 0;
        OuterSubshell outer;
        orb_real_t baseScale = orb_real_t(0);
    };

    /// Build `count` points of the fixed benchmark atom, then render+present kBenchFramesPerStep
    /// frames through the SAME renderSceneGrouped() path production code uses (fade, proton
    /// marker, per-subshell shell coloring, buzz flicker) plus the title/scale-bar overlay every
    /// real frame draws -- a uniform-white/no-overlay shortcut would measure a cheaper pipeline
    /// than what actually ships.
    StepStats runStep(Display &display, AtomPoint *points, PointGroup *groups, int count, CameraState &camera)
    {
        StepStats stats;
        stats.points = count;

        int64_t buildStartUs = esp_timer_get_time();
        AtomSubshellRange ranges[kMaxConfigSubshells];
        int rangeCount = 0;
        [[maybe_unused]] ElectronConfig config =
            buildAtomPointCloud(kBenchAtomicNumber, points, count, kBenchSeed, ranges, &rangeCount);
        stats.outer = outerSubshellRRef(points, ranges, rangeCount);
        uint16_t subshellColors[kMaxConfigSubshells];
        colorizeAtomSubshells(ranges, rangeCount, stats.outer, subshellColors);
        int groupCount = rangeCount;
        for (int s = 0; s < rangeCount; s++)
            groups[s] = PointGroup{ranges[s].startIndex, ranges[s].count, subshellColors[s]};
        stats.buildMs = (esp_timer_get_time() - buildStartUs) / 1000;

        AtomScale atomScale = scaleForAtom(stats.outer.rRef);
        stats.baseScale = atomScale.baseScale;

        char titleText[32];
        std::snprintf(titleText, sizeof(titleText), "%s (Z=%d)", elementSymbol(kBenchAtomicNumber),
                      kBenchAtomicNumber);

        double waitMsAccum = 0, renderMsAccum = 0;
        double minRenderMs = -1, maxRenderMs = 0;
        int64_t windowStartUs = esp_timer_get_time();

        for (int f = 0; f < kBenchFramesPerStep; f++)
        {
            int64_t tBeforeWait = esp_timer_get_time();
            display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
            int64_t tAfterWait = esp_timer_get_time();

            renderSceneGrouped(display.getFrameBuf(), points, groups, groupCount, kProtonColor, camera,
                               stats.baseScale, uint32_t(f), kHiddenPointsThreshold);
            drawText(display.getFrameBuf(), kTitleTextX, kTitleTextY, titleText, kTextColor, kFontLarge);
            drawScaleBar(display.getFrameBuf(), stats.baseScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

            display.presentFrame();
            int64_t tAfterPresent = esp_timer_get_time();
            stepCamera(&camera);

            double waitMs = double(tAfterWait - tBeforeWait) / 1000.0;
            double renderMs = double(tAfterPresent - tAfterWait) / 1000.0;
            waitMsAccum += waitMs;
            renderMsAccum += renderMs;
            if (minRenderMs < 0 || renderMs < minRenderMs)
                minRenderMs = renderMs;
            if (renderMs > maxRenderMs)
                maxRenderMs = renderMs;

            vTaskDelay(pdMS_TO_TICKS(1));
        }

        double elapsedS = double(esp_timer_get_time() - windowStartUs) / 1e6;
        stats.avgWaitMs = waitMsAccum / kBenchFramesPerStep;
        stats.avgRenderMs = renderMsAccum / kBenchFramesPerStep;
        stats.minRenderMs = minRenderMs;
        stats.maxRenderMs = maxRenderMs;
        stats.fps = elapsedS > 0 ? double(kBenchFramesPerStep) / elapsedS : 0.0;
        return stats;
    }

    /// Human-readable heap dump (totals + min-free-ever, not just the CSV snapshot logMemory()
    /// takes) -- logged once at the start of the sweep, since min-free-ever is only meaningful
    /// as a "worst point up to now" figure once the sweep's allocations have had a chance to
    /// run.
    void printMemoryInfo()
    {
        multi_heap_info_t info;
        heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
        ESP_LOGI(kBenchmarkTag, "heap: %u/%u bytes free/total, %u largest block, %u min free ever",
                 info.total_free_bytes, info.total_allocated_bytes + info.total_free_bytes, info.largest_free_block,
                 info.minimum_free_bytes);
        ESP_LOGI(kBenchmarkTag, "internal RAM: %u/%u bytes free/total, %u largest block, %u min free ever",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        ESP_LOGI(kBenchmarkTag, "external RAM: %u/%u bytes free/total, %u largest block, %u min free ever",
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    }
} // namespace

/// Log internal-RAM and PSRAM free/largest-block bytes as one BENCH,MEM CSV line, `label`
/// tagging which point in the run this snapshot is from -- lets a run's own start/end (and
/// runs across code revisions, e.g. before/after a memory-layout change) be diffed for
/// headroom regressions.
void logMemory(const char *label)
{
    ESP_LOGI(kBenchmarkTag, "BENCH,MEM,%s,internal_free,%u,internal_largest,%u,psram_free,%u,psram_largest,%u",
             label, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

void runBenchmarkTest(Display &display)
{
    // EXT_RAM_BSS_ATTR -- PSRAM, not internal RAM: same reasoning as atom_view.cpp's
    // AtomPresetState (a same-order-of-magnitude points buffer), which aborted at boot when
    // placed in internal RAM because it left too little contiguous space for Display::
    // Display()'s own DMA-capable frame-buffer allocation.
    static EXT_RAM_BSS_ATTR AtomPoint points[kAtomNumPoints];
    static PointGroup groups[kMaxConfigSubshells]; // tiny (<=20 entries) -- no PSRAM need
    CameraState camera;

    const char *symbol = elementSymbol(kBenchAtomicNumber);
    ESP_LOGI(kBenchmarkTag, "BENCH,START,element,%s,Z,%d,frames_per_step,%d", symbol, kBenchAtomicNumber,
             kBenchFramesPerStep);
    logMemory("start"); // with the static points/groups buffers already reserved above
    printMemoryInfo();

    // Correctness fingerprint, part 1: the electron configuration and per-subshell Z_eff are
    // pure functions of Z (no point sampling, no RNG) -- identical every run on correct code,
    // and directly comparable against tools/orbitals_host/gen_atom_reference.py's host
    // reference for Fe (see this file's header comment). Logged once, not per sweep step.
    ElectronConfig config = electronConfiguration(kBenchAtomicNumber);
    for (int i = 0; i < config.count; i++)
    {
        int n = config.subshells[i].n, ell = config.subshells[i].ell, occ = config.subshells[i].occ;
        ESP_LOGI(kBenchmarkTag, "BENCH,CONFIG,%s,%d,%d,%d", symbol, n, ell, occ);
    }
    for (int i = 0; i < config.count; i++)
    {
        int n = config.subshells[i].n, ell = config.subshells[i].ell;
        orb_real_t zEff = zEffRadial(kBenchAtomicNumber, config, n, ell);
        ESP_LOGI(kBenchmarkTag, "BENCH,ZEFF,%s,%d,%d,%.17g", symbol, n, ell, double(zEff));
    }

    StepStats results[kBenchNumSteps];
    for (int i = 0; i < kBenchNumSteps; i++)
    {
        int count = kBenchPointCounts[i];
        results[i] = runStep(display, points, groups, count, camera);
        const StepStats &s = results[i];

        ESP_LOGI(kBenchmarkTag,
                 "BENCH,STEP,points,%d,build_ms,%lld,avg_render_ms,%.3f,min_render_ms,%.3f,max_render_ms,%.3f,"
                 "avg_wait_ms,%.3f,fps,%.2f",
                 s.points, s.buildMs, s.avgRenderMs, s.minRenderMs, s.maxRenderMs, s.avgWaitMs, s.fps);

        // Correctness fingerprint, part 2: the outer (largest p90-radius) occupied subshell and
        // its reference radius come from the actual sampled points -- deterministic for a fixed
        // (Z, count, seed), so a change here at a given point count flags a regression somewhere
        // in the radial-sampling/Z_eff/scale pipeline that part 1's pure-Z numbers can't see.
        ESP_LOGI(kBenchmarkTag, "BENCH,GEOM,points,%d,outer_n,%d,outer_ell,%d,outer_rref_bohr,%.6f,base_scale_px,%.6f",
                 s.points, s.outer.n, s.outer.ell, double(s.outer.rRef), double(s.baseScale));
    }

    ESP_LOGI(kBenchmarkTag, "-- summary (%s, Z=%d) --", symbol, kBenchAtomicNumber);
    ESP_LOGI(kBenchmarkTag, "%8s %10s %11s %11s %11s %8s", "points", "build_ms", "avg_render", "min_render",
             "max_render", "fps");
    for (int i = 0; i < kBenchNumSteps; i++)
    {
        const StepStats &s = results[i];
        ESP_LOGI(kBenchmarkTag, "%8d %10lld %9.3fms %9.3fms %9.3fms %8.2f", s.points, s.buildMs, s.avgRenderMs,
                 s.minRenderMs, s.maxRenderMs, s.fps);
    }
    logMemory("end"); // diff against the "start" snapshot to catch fragmentation/leaks across the sweep
    ESP_LOGI(kBenchmarkTag, "BENCH,DONE");
}
