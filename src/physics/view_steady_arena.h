/**
 * @file view_steady_arena.h
 * @brief Shared steady-state point/color storage for the two LIVE views:
 *        orbital_view.cpp's OrbitalPresetState (points/colors/resample.psi2Sorted) and
 *        atom_view.cpp's AtomPresetState (points). Everything else in those structs (title,
 *        config, ranges[], groups[], baseScale, etc.) stays embedded normally -- cheap
 *        fixed-size metadata (kMaxConfigSubshells=20-ish entries), not per-point arrays, so
 *        there's nothing to gain by sharing it.
 *
 *        ONLY the two live-view instances (runOrbitalView()'s/runAtomView()'s own `preset`
 *        statics, views/orbital_view.cpp and views/atom_view.cpp) bind their points/colors
 *        pointers to this arena, each doing so explicitly and exactly once (see those
 *        functions). debug/screenshot_batch.cpp's and debug/gif_capture_test.cpp's OWN
 *        separate OrbitalPresetState/AtomPresetState instances deliberately do NOT: they bind
 *        to their own private backing arrays instead. That separation matters because
 *        screenshot_batch.cpp's captures run from the screenshot console's task, which can be
 *        live at the same time as the main render loop's task (see debug/screenshot_pause.h)
 *        -- if those instances shared this arena too, a screenshot batch capture would
 *        silently overwrite whatever preset the live view is currently showing, so when the
 *        live view resumes after the capture it would jump to displaying the capture's last
 *        preset instead of the user's own.
 *
 *        Why sharing is safe between the two LIVE views specifically: main.cpp's CYD boot
 *        loop runs runOrbitalView()/runAtomView() strictly sequentially, never concurrently,
 *        and on the S3 the chooser only ever has one of the two active at a time either --
 *        see config/visual_constants.h's kOrbitalNumPoints/kAtomNumPoints comment for the
 *        measured numbers this made possible.
 *
 *        Trade-off this introduces: since the two live views now share physical storage,
 *        re-entering one after the other has run can no longer assume its own points/colors
 *        are still what they were -- both views now reload (points/colors freshly rebuilt,
 *        tens to ~200ms) every time they're (re-)entered, rather than the old "first-ever-call
 *        only" reload guard that let a later re-entry instantly resume whatever was last
 *        shown. The resulting preset is visually IDENTICAL either way (same remembered
 *        index/Z, same fixed RNG seed) -- the only user-visible cost is that brief reload
 *        pause. See runOrbitalView()'s/runAtomView()'s own comments for exactly where this
 *        reload now happens.
 *
 *        Deliberately the SAME implementation on both boards (no CONFIG_IDF_TARGET_ESP32
 *        split) -- see physics/view_scratch_arena.h's identical reasoning for why.
 */
#pragma once

#include "config/visual_constants.h" // kOrbitalNumPoints, kAtomNumPoints
#include "esp_attr.h"                 // EXT_RAM_BSS_ATTR
#include "physics/atom_cloud.h"      // AtomPoint
#include "physics/orbitals.h"        // orb_real_t
#include "physics/pointcloud.h"      // OrbitalPoint

union ViewSteadyArena
{
    struct
    {
        OrbitalPoint points[kOrbitalNumPoints];
        uint16_t colors[kOrbitalNumPoints];
        orb_real_t psi2Sorted[kOrbitalNumPoints];
    } orbital;
    struct
    {
        AtomPoint points[kAtomNumPoints];
    } atom;
};

/// One shared instance -- see this file's header comment for who binds to it (only the two
/// live views) and who deliberately doesn't (screenshot_batch.cpp/gif_capture_test.cpp's own
/// separate preset instances). Same "function-local static in a header-defined inline
/// function" sharing idiom as physics/view_scratch_arena.h's viewScratchArena() and
/// physics/hfs_radial.h's shared RadialTable scratch.
/// EXT_RAM_BSS_ATTR -- PSRAM on the S3 (this arena is tens of KB at kOrbitalNumPoints=12000,
/// same reasoning as OrbitalPresetState/AtomPresetState themselves used to carry
/// individually); falls back to internal SRAM on CYD (no PSRAM), which is the whole point of
/// sharing it in the first place.
inline ViewSteadyArena &viewSteadyArena()
{
    static EXT_RAM_BSS_ATTR ViewSteadyArena arena;
    return arena;
}
