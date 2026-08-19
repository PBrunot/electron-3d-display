/**
 * @file screenshot_pause.h
 * @brief Cooperative pause gate so a screenshot (single-shot or batch) never runs
 *        concurrently with the main render loop.
 *
 * Both sides touch state that isn't safe to access from two tasks at once:
 *  - The live Display (frame buffer + present semaphore) -- the single-shot 's'/SS_CAP
 *    capture reads display.getFrameBuf() directly (see screenshot_console.h).
 *  - Point-cloud-building scratch: OrbitalPresetState::load()/AtomPresetState::load() (via
 *    computeOrbitalLevels()/scaleFromRadii() in orbital_presets.cpp and outerSubshellRRef()
 *    in atom_cloud.cpp) write into `static` scratch arrays (order[]/radii[]/
 *    sSubshellRadii[]) that are shared by EVERY preset-state instance, not private per
 *    object -- deliberately static to avoid huge per-call stack/heap churn, but that means
 *    two concurrent load()s, one from the main task's live viewer and one from the console's
 *    'a'/SS_CAP_ALL batch capture (screenshot_batch.cpp), would corrupt each other's
 *    in-flight work even though screenshot_batch.cpp renders into its own private frame
 *    buffer.
 *
 * init() must run once at boot, before the console task starts. Every long-running render
 * loop (chooser.cpp's runChooser(), orbital_view.cpp's/atom_view.cpp's steady-state loops)
 * calls checkpoint() once per iteration; screenshot_console.cpp calls
 * requestPause()/releasePause() around any capture. Short bounded transitions (fly-overs,
 * the shell-dissection sequence) do NOT call checkpoint(), so requestPause() may block up to
 * roughly one such transition's duration (a couple of seconds worst case) before the pause
 * takes effect -- callers should use a generous timeout, not assume an instant pause, and
 * MUST check the return value rather than proceeding unpaused on a timeout.
 */
#pragma once

#include <cstdint>

namespace screenshot_pause
{
    /// Creates the underlying semaphores. Call once at boot, before any render loop or the
    /// console task starts.
    void init();

    /// Call once per iteration from every long-running render loop, at a point where no
    /// frame is half-drawn (e.g. right after presentFrame(), before touching frameBuf
    /// again). Near-free (a single atomic load) when no pause is pending; blocks until
    /// releasePause() otherwise.
    void checkpoint();

    /// Requests the active render loop pause at its next checkpoint() and blocks until it
    /// acknowledges (or timeoutMs elapses). Returns true once paused -- callers MUST pair a
    /// true result with a later releasePause(), and MUST NOT touch shared render state on a
    /// false result (the render loop never paused, so nothing is actually safe to touch).
    bool requestPause(uint32_t timeoutMs);

    /// Lets the paused render loop resume. Only call after a requestPause() that returned
    /// true.
    void releasePause();
} // namespace screenshot_pause
