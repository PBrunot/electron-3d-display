/**
 * @file view_scratch_arena.h
 * @brief Shared scratch storage for orbital_view.cpp's OrbitalPresetState::load() working
 *        buffers (psi2/signs/levels), orbital_presets.cpp's computeOrbitalLevels()/
 *        scaleFromRadii() order/radii scratch, and atom_cloud.cpp's per-subshell radius
 *        scratch (sSubshellRadii, used by outerSubshellRRef()/subshellDissectionPlan()).
 *
 *        Why this is safe: main.cpp's CYD boot loop runs runOrbitalView()/runAtomView()
 *        strictly sequentially, never concurrently, and on the S3 the chooser menu only ever
 *        has one of the two views active at a time either (see config/visual_constants.h's
 *        kOrbitalNumPoints comment). Every array unioned below is pure "write, read, discard
 *        within one call" scratch, never expected to survive past the call that filled it --
 *        orbital's three load-only arrays are only touched from within
 *        OrbitalPresetState::load() (called only while runOrbitalView() is on the stack), and
 *        atom's sSubshellRadii is only touched from within runAtomView()'s call tree. So one
 *        shared arena, sized to the larger of the two views' peak need, safely replaces two
 *        separately-reserved regions -- same idiom as the OrderRadiiScratch union below
 *        (order[]/radii[] never needed at the same time within one load() call), just
 *        extended across the orbital/atom module boundary.
 *
 *        Deliberately the SAME implementation on both boards (no CONFIG_IDF_TARGET_ESP32
 *        split): on the S3 (PSRAM, no tight-budget reason to need this) sharing the arena
 *        costs nothing meaningful, and one code path exercised on both boards is safer than a
 *        second, S3-only path that never gets the same real-hardware testing CYD gets.
 */
#pragma once

#include "config/visual_constants.h" // kOrbitalNumPoints, kAtomNumPoints
#include "physics/orbitals.h"        // orb_real_t
#include "esp_attr.h"                 // EXT_RAM_BSS_ATTR

/// orbital_presets.cpp's computeOrbitalLevels()/scaleFromRadii() scratch: both are called
/// sequentially within one OrbitalPresetState::load(), each fully consuming its own use
/// (write, read via std::sort/nth_element, discard) before the other runs -- one buffer,
/// reinterpreted as whichever type is needed, replaces two separate kOrbitalNumPoints-sized
/// arrays.
union OrderRadiiScratch
{
    int order[kOrbitalNumPoints];
    orb_real_t radii[kOrbitalNumPoints];
};

/// orbital_view.cpp's OrbitalPresetState::load()-only scratch, plus the order/radii scratch
/// above. These four members ARE siblings (not further unioned against each other): within
/// one load() call, computeOrbitalLevels() reads psi2[] while concurrently using
/// orderOrRadii.order[] as its own working array, and signs[]/levels[] both stay alive from
/// buildOrbitalPointCloud() through the colors[] loop that follows computeOrbitalLevels() --
/// so this struct's total footprint (10 bytes/point) is what has to coexist with atom's
/// scratch need at the ViewScratchArena level below, not something further shrinkable here.
struct OrbitalLoadScratch
{
    orb_real_t psi2[kOrbitalNumPoints];
    int8_t signs[kOrbitalNumPoints];
    uint8_t levels[kOrbitalNumPoints];
    OrderRadiiScratch orderOrRadii;
};

/// Top-level union: OrbitalLoadScratch (alive only during OrbitalPresetState::load(), i.e.
/// only while runOrbitalView() is on the stack) vs. atom_cloud.cpp's sSubshellRadii scratch
/// (alive only while runAtomView() is on the stack) -- never needed concurrently, see this
/// file's header comment.
union ViewScratchArena
{
    OrbitalLoadScratch orbital;
    orb_real_t atomSubshellRadii[kAtomNumPoints];
};

/// One shared instance, safe to call from any translation unit that includes this header: a
/// function-local static inside an inline function defined in a header has exactly one
/// instance program-wide -- the same guarantee physics/hfs_radial.h's shared RadialTable
/// scratch already relies on (see that header's buildRadialSamplerRuntime() docstring).
/// EXT_RAM_BSS_ATTR -- PSRAM on the S3 (this arena is tens of KB at kOrbitalNumPoints=12000,
/// same reasoning as OrbitalPresetState/AtomPresetState themselves); falls back to internal
/// SRAM on CYD (no PSRAM), which is the whole point of sharing it in the first place.
inline ViewScratchArena &viewScratchArena()
{
    static EXT_RAM_BSS_ATTR ViewScratchArena arena;
    return arena;
}
