#include "debug/benchmark_test.h"

#include <cstdio>

#include "physics/atom_cloud.h"
#include "physics/orbital_library.h"
#include "physics/orbital_presets.h"
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
#include "sdkconfig.h" // CONFIG_IDF_TARGET_ESP32

static const char *kBenchmarkTag = "benchmark";

// ============================================================================================
// Tunable constants
// ============================================================================================

/// Fixed element used at every atom sweep step, so the only thing varying between rows is
/// point count. Matches atom_view_test.cpp's choice (Fe: enough occupied subshells -- 1s..3d6
/// -- to be a representative multi-shell cloud, not a trivial single-shell case) and is also
/// one of atom_validation_test.cpp's kValidationZs, so the CONFIG/ZEFF numbers logged below
/// are directly comparable against tools/orbitals_host/gen_atom_reference.py's host reference.
static constexpr int kBenchAtomicNumber = 26; // Fe

/// Fixed orbital preset used at every orbital sweep step -- kOrbitalLibrary's own default
/// (2pz, see orbital_view.cpp), so the numbers are representative of what orbital_view.cpp
/// actually shows on boot, not a cherry-picked shape.
static constexpr int kBenchOrbitalPreset = kOrbitalDefaultPresetIndex;

/// Point counts swept, ascending, for both the atom and orbital sweeps. On the S3,
/// kAtomNumPoints == kOrbitalNumPoints (config/visual_constants.h) is the real production
/// count both live viewers use, so it's the ceiling here too -- also sizes the static point
/// buffers below, no reallocation between steps. On CYD (no PSRAM) the two ceilings differ
/// and are both far smaller (kAtomNumPoints=1000, kOrbitalNumPoints=3400) -- since this same
/// array drives both sweeps, it must stay <= the SMALLER of the two (1000) or the atom sweep
/// would write past the end of the static atomPoints[] buffer below.
#if CONFIG_IDF_TARGET_ESP32
static constexpr int kBenchPointCounts[] = {200, 400, 600, 800, 1000};
#else
static constexpr int kBenchPointCounts[] = {500, 1000, 2000, 4000, 8000};
#endif
static constexpr int kBenchNumSteps = int(sizeof(kBenchPointCounts) / sizeof(kBenchPointCounts[0]));

/// Ceiling for sizing the sweep's own static point/scratch buffers below -- the largest step
/// actually swept (last element, ascending), NOT kAtomNumPoints/kOrbitalNumPoints. On the S3
/// these are equal (8000 either way). On CYD they'd overshoot: sizing to kOrbitalNumPoints=3400
/// plus this file's extra orbital scratch arrays (psi2/signs/levels/psi2Sorted -- none of which
/// OrbitalPresetState needs) adds ~34KB of internal-SRAM BSS beyond what normal boot's own view
/// state already uses, on a board with no PSRAM fallback -- confirmed on hardware to fragment
/// internal SRAM enough that Display::Display()'s 240x320 DMA frame-buffer allocation fails
/// outright (abort() at boot, even at the allocator's smallest 1-row/block retry).
static constexpr int kBenchMaxPoints = kBenchPointCounts[kBenchNumSteps - 1];

static constexpr uint32_t kBenchSeed = kAtomCloudSeed; // same seed atom_view.cpp uses -- reproducible sweep

/// Frames rendered and timed per point-count step. Large enough to average out one-off
/// scheduling jitter without making the whole sweep take unreasonably long to capture.
static constexpr int kBenchFramesPerStep = 60;

// ============================================================================================

namespace
{
    /// Performance numbers for one sweep step, shared shape for both the atom and orbital
    /// sweeps so their summary tables line up column-for-column.
    struct StepStats
    {
        int points = 0;
        int64_t buildMs = 0; // point-cloud sampling ("time to load and compute points")
        double avgRenderMs = 0, minRenderMs = 0, maxRenderMs = 0; // "time to render" -- CPU draw + presentFrame() kick-off
        double avgWaitMs = 0;                                     // "time to prepare the frame" -- blocked on the previous frame's DMA
        double fps = 0;
        uint32_t iramFreeBytes = 0; // internal-RAM free at the end of this step
    };

    struct AtomStepStats
    {
        StepStats perf;
        OuterSubshell outer;
        orb_real_t baseScale = orb_real_t(0);
    };

    /// Build `count` points of the fixed benchmark atom, then render+present kBenchFramesPerStep
    /// frames through the SAME renderSceneGrouped() path production code uses (fade, proton
    /// marker, per-subshell shell coloring, buzz flicker) plus the title/scale-bar overlay every
    /// real frame draws -- a uniform-white/no-overlay shortcut would measure a cheaper pipeline
    /// than what actually ships.
    AtomStepStats runAtomStep(Display &display, AtomPoint *points, PointGroup *groups, int count, CameraState &camera)
    {
        AtomStepStats stats;
        stats.perf.points = count;

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
        stats.perf.buildMs = (esp_timer_get_time() - buildStartUs) / 1000;

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

            renderSceneGrouped(display, points, groups, groupCount, kProtonColor, camera, stats.baseScale,
                               uint32_t(f), kHiddenPointsThreshold);
            drawText(display, kTitleTextX, kTitleTextY, titleText, kTextColor, kFontLarge);
            drawScaleBar(display, stats.baseScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

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
        stats.perf.avgWaitMs = waitMsAccum / kBenchFramesPerStep;
        stats.perf.avgRenderMs = renderMsAccum / kBenchFramesPerStep;
        stats.perf.minRenderMs = minRenderMs;
        stats.perf.maxRenderMs = maxRenderMs;
        stats.perf.fps = elapsedS > 0 ? double(kBenchFramesPerStep) / elapsedS : 0.0;
        stats.perf.iramFreeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        return stats;
    }

    /// Orbital-sweep counterpart to runAtomStep(): builds `count` points of the fixed benchmark
    /// preset, then render+present kBenchFramesPerStep frames through the same renderScene() +
    /// title/quantum-numbers/scale-bar overlay orbital_view.cpp's renderOrbitalFrame() draws
    /// every real frame -- reimplemented here (rather than calling renderOrbitalFrame()
    /// directly) because that helper always draws OrbitalPresetState's fixed kOrbitalNumPoints
    /// count, not this sweep's variable `count`.
    StepStats runOrbitalStep(Display &display, OrbitalPoint *points, uint16_t *colors, orb_real_t *psi2Scratch,
                             int8_t *signsScratch, uint8_t *levelsScratch, orb_real_t *psi2SortedScratch, int count,
                             CameraState &camera)
    {
        StepStats stats;
        stats.points = count;

        const OrbitalDescriptor &d = kOrbitalLibrary[kBenchOrbitalPreset];
        char title[kMaxOrbitalTitleLen];
        formatOrbitalTitle(title, sizeof(title), d.n, d.ell, d.m);
        char orbitalNumbers[32];
        std::snprintf(orbitalNumbers, sizeof(orbitalNumbers), "n=%d %s=%d m=%d", d.n, kGlyphScriptL, d.ell, d.m);

        int64_t buildStartUs = esp_timer_get_time();
        XorShift32 rng(1); // scratch only -- buildOrbitalPointCloud() re-seeds its own RNG from kBenchSeed below
        orb_real_t radialCoeff[kOrbitalNMax];
        orb_real_t legendreCoeff[kOrbitalEllMax + 1];
        buildOrbitalPointCloud(d.n, d.ell, d.m, points, psi2Scratch, signsScratch, count, kBenchSeed, &rng,
                               radialCoeff, legendreCoeff);
        computeOrbitalLevels(psi2Scratch, count, levelsScratch, psi2SortedScratch);
        for (int i = 0; i < count; i++)
            colors[i] = orbitalLevelToColor565(levelsScratch[i], signsScratch[i], d.posRgb565, d.negRgb565);
        stats.buildMs = (esp_timer_get_time() - buildStartUs) / 1000;

        OrbitalScale scale = scaleFromRadii(points, count);

        double waitMsAccum = 0, renderMsAccum = 0;
        double minRenderMs = -1, maxRenderMs = 0;
        int64_t windowStartUs = esp_timer_get_time();

        for (int f = 0; f < kBenchFramesPerStep; f++)
        {
            int64_t tBeforeWait = esp_timer_get_time();
            display.waitForFlushDone(); // previous frame's DMA must finish before frameBuf is overwritten
            int64_t tAfterWait = esp_timer_get_time();

            renderScene(display, points, colors, count, kProtonColor, camera, scale.baseScale, uint32_t(f),
                       kHiddenPointsThreshold);
            drawProtonMarker(display, kProtonColor, kOrbitalProtonMarkerSize);
            drawText(display, kTitleTextX, kTitleTextY, title, kTextColor, kFontHuge);
            int width = textWidth(orbitalNumbers, kFontLarge);
            drawText(display, Display::kDisplayWidth - width,
                    Display::kDisplayHeight - kFontLarge.height - 15, orbitalNumbers, kTextColor, kFontLarge);
            drawScaleBar(display, scale.baseScale / kPmPerBohr, "pm", kScaleBarColor, kTextColor);

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
        stats.iramFreeBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
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

    void logStep(const char *kind, const StepStats &s)
    {
        ESP_LOGI(kBenchmarkTag,
                 "BENCH,STEP,kind,%s,points,%d,build_ms,%lld,avg_render_ms,%.3f,min_render_ms,%.3f,max_render_ms,%.3f,"
                 "avg_wait_ms,%.3f,fps,%.2f,iram_free,%u",
                 kind, s.points, s.buildMs, s.avgRenderMs, s.minRenderMs, s.maxRenderMs, s.avgWaitMs, s.fps,
                 (unsigned)s.iramFreeBytes);
    }

    void printSummaryTable(const char *kind, const StepStats *results, int count)
    {
        ESP_LOGI(kBenchmarkTag, "-- %s performance --", kind);
        ESP_LOGI(kBenchmarkTag, "%8s %10s %11s %11s %11s %8s %10s", "points", "build_ms", "avg_render", "min_render",
                 "max_render", "fps", "iram_free");
        for (int i = 0; i < count; i++)
        {
            const StepStats &s = results[i];
            ESP_LOGI(kBenchmarkTag, "%8d %10lld %9.3fms %9.3fms %9.3fms %8.2f %10u", s.points, s.buildMs,
                     s.avgRenderMs, s.minRenderMs, s.maxRenderMs, s.fps, (unsigned)s.iramFreeBytes);
        }
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
    // Display()'s own DMA-capable frame-buffer allocation. Sized to kBenchMaxPoints (the
    // sweep's own largest step), not kAtomNumPoints -- see that constant's doc comment.
    static EXT_RAM_BSS_ATTR AtomPoint atomPoints[kBenchMaxPoints];
    static PointGroup atomGroups[kMaxConfigSubshells]; // tiny (<=20 entries) -- no PSRAM need

    // Orbital sweep's own buffers, same PSRAM/kBenchMaxPoints reasoning -- OrbitalPresetState
    // (orbital_view.h) places its equivalents in PSRAM for the same reason.
    static EXT_RAM_BSS_ATTR OrbitalPoint orbitalPoints[kBenchMaxPoints];
    static EXT_RAM_BSS_ATTR uint16_t orbitalColors[kBenchMaxPoints];
    static EXT_RAM_BSS_ATTR orb_real_t orbitalPsi2[kBenchMaxPoints];      // scratch, see runOrbitalStep()
    static EXT_RAM_BSS_ATTR int8_t orbitalSigns[kBenchMaxPoints];        // scratch
    static EXT_RAM_BSS_ATTR uint8_t orbitalLevels[kBenchMaxPoints];      // scratch
    static EXT_RAM_BSS_ATTR orb_real_t orbitalPsi2Sorted[kBenchMaxPoints]; // scratch

    CameraState camera;

    const char *atomSymbol = elementSymbol(kBenchAtomicNumber);
    const OrbitalDescriptor &orbitalPreset = kOrbitalLibrary[kBenchOrbitalPreset];
    ESP_LOGI(kBenchmarkTag, "BENCH,START,atom,%s,Z,%d,orbital,%s,frames_per_step,%d", atomSymbol, kBenchAtomicNumber,
             orbitalPreset.label, kBenchFramesPerStep);
    logMemory("start"); // with the static point/color/scratch buffers already reserved above
    printMemoryInfo();

    // Correctness fingerprint, part 1: the electron configuration and per-subshell Z_eff are
    // pure functions of Z (no point sampling, no RNG) -- identical every run on correct code,
    // and directly comparable against tools/orbitals_host/gen_atom_reference.py's host
    // reference for Fe (see this file's header comment). Logged once, not per sweep step.
    ElectronConfig config = electronConfiguration(kBenchAtomicNumber);
    for (int i = 0; i < config.count; i++)
    {
        int n = config.subshells[i].n, ell = config.subshells[i].ell, occ = config.subshells[i].occ;
        ESP_LOGI(kBenchmarkTag, "BENCH,CONFIG,%s,%d,%d,%d", atomSymbol, n, ell, occ);
    }
    for (int i = 0; i < config.count; i++)
    {
        int n = config.subshells[i].n, ell = config.subshells[i].ell;
        orb_real_t zEff = zEffRadial(kBenchAtomicNumber, config, n, ell);
        ESP_LOGI(kBenchmarkTag, "BENCH,ZEFF,%s,%d,%d,%.17g", atomSymbol, n, ell, double(zEff));
    }

    // ---- Atom sweep -----------------------------------------------------------------------
    StepStats atomResults[kBenchNumSteps];
    for (int i = 0; i < kBenchNumSteps; i++)
    {
        int count = kBenchPointCounts[i];
        AtomStepStats step = runAtomStep(display, atomPoints, atomGroups, count, camera);
        atomResults[i] = step.perf;
        logStep("atom", step.perf);

        // Correctness fingerprint, part 2: the outer (largest p90-radius) occupied subshell and
        // its reference radius come from the actual sampled points -- deterministic for a fixed
        // (Z, count, seed), so a change here at a given point count flags a regression somewhere
        // in the radial-sampling/Z_eff/scale pipeline that part 1's pure-Z numbers can't see.
        ESP_LOGI(kBenchmarkTag, "BENCH,GEOM,points,%d,outer_n,%d,outer_ell,%d,outer_rref_bohr,%.6f,base_scale_px,%.6f",
                 step.perf.points, step.outer.n, step.outer.ell, double(step.outer.rRef), double(step.baseScale));
    }

    // ---- Orbital sweep ----------------------------------------------------------------------
    StepStats orbitalResults[kBenchNumSteps];
    for (int i = 0; i < kBenchNumSteps; i++)
    {
        int count = kBenchPointCounts[i];
        orbitalResults[i] = runOrbitalStep(display, orbitalPoints, orbitalColors, orbitalPsi2, orbitalSigns,
                                           orbitalLevels, orbitalPsi2Sorted, count, camera);
        logStep("orbital", orbitalResults[i]);
    }

    printSummaryTable("atom", atomResults, kBenchNumSteps);
    printSummaryTable("orbital", orbitalResults, kBenchNumSteps);

    logMemory("end"); // diff against the "start" snapshot to catch fragmentation/leaks across the sweep
    ESP_LOGI(kBenchmarkTag, "BENCH,DONE");
}
