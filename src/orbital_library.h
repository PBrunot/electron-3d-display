// Fixed set of selectable hydrogen orbitals, each with its OrbitalSampler
// built entirely at compile time (see pointcloud.h's buildOrbitalSamplerConstexpr()
// and the CLAUDE.md M2 note on why this project avoids on-device recomputation).
// This is the M4 step in CLAUDE.md §7: "libreria di orbitali... selezionabili" --
// picking among these is the whole job of the selection UI/IMU work in M3/M4,
// not covered here.
//
// The real p_x/p_y/p_z and d-orbital shapes fall directly out of psiReal()'s
// existing convention (cos(m*phi) for m>=0, sin(|m|*phi) for m<0) for the usual
// m=+-1 (p) / m=+-2 (d) choices -- no separate "real spherical harmonic" math
// needed, see orbitals.h's psiReal() docstring.
#pragma once

#include <array>
#include <cstdint>

#include "pointcloud.h"

struct OrbitalDescriptor
{
    int n, ell, m;
    const char *label;
    // Phase-color pair (positive lobe, negative lobe) -- sign of psi_real
    // determines the color, and EVERY orbital uses the same classic vibrant
    // orange/blue pair ("all orbitals should be colored according to sign
    // of psi_Real with the classical blue/orange vibrant colors",
    // 2026-08-17). Superseded the older per-preset scheme (each orbital had
    // its own distinguishing hue) since that broke the sign-color
    // association a viewer would otherwise learn to read at a glance across
    // the whole library. Index-matched to micropython/cloud_common.py's
    // ORBITAL_PHASE_COLORS; keep the two in lockstep.
    uint8_t posRgb[3];
    uint8_t negRgb[3];
};

// Index-matched to micropython/cloud_common.py's ORBITAL_PRESETS (same order, same 16
// entries) so preset N here is the same orbital as preset N there -- kept that way on
// purpose for cross-port parity/debugging, not just coincidence. Index 15 was
// historically mislabeled "5pz3"/"5p_z3" in cloud_common.py despite (n=5,ell=2,m=0)
// being a 5d_z2 orbital by its own quantum numbers; corrected here and there in lockstep
// (5dz2/5d_z2).
constexpr OrbitalDescriptor kOrbitalLibrary[] = {
    {1, 0, 0, "1s",      {255, 120, 40}, {0, 0, 255}},   // 0
    {2, 0, 0, "2s",      {255, 120, 40}, {0, 0, 255}},   // 1
    {2, 1, 1, "2px",     {255, 120, 40}, {0, 0, 255}},   // 2
    {2, 1, -1, "2py",    {255, 120, 40}, {0, 0, 255}},   // 3
    {2, 1, 0, "2pz",     {255, 120, 40}, {0, 0, 255}},   // 4 -- DEFAULT_PRESET_INDEX
    {3, 0, 0, "3s",      {255, 120, 40}, {0, 0, 255}},   // 5
    {3, 1, 1, "3px",     {255, 120, 40}, {0, 0, 255}},   // 6
    {3, 1, -1, "3py",    {255, 120, 40}, {0, 0, 255}},   // 7
    {3, 1, 0, "3pz",     {255, 120, 40}, {0, 0, 255}},   // 8
    {3, 2, 0, "3dz2",    {255, 120, 40}, {0, 0, 255}},   // 9
    {3, 2, 1, "3dxz",    {255, 120, 40}, {0, 0, 255}},   // 10
    {3, 2, -1, "3dyz",   {255, 120, 40}, {0, 0, 255}},   // 11
    {3, 2, 2, "3dx2-y2", {255, 120, 40}, {0, 0, 255}},   // 12
    {3, 2, -2, "3dxy",   {255, 120, 40}, {0, 0, 255}},   // 13
    {4, 3, 0, "4fz3",    {255, 120, 40}, {0, 0, 255}},   // 14
    {5, 2, 0, "5dz2",    {255, 120, 40}, {0, 0, 255}},   // 15 -- was "5pz3", corrected
};
constexpr int kOrbitalLibraryCount = sizeof(kOrbitalLibrary) / sizeof(kOrbitalLibrary[0]);
constexpr int kOrbitalDefaultPresetIndex = 4; // 2pz, matches cloud_common.DEFAULT_PRESET_INDEX

constexpr std::array<OrbitalSampler, kOrbitalLibraryCount> buildOrbitalLibrarySamplers()
{
    std::array<OrbitalSampler, kOrbitalLibraryCount> samplers{};
    for (int i = 0; i < kOrbitalLibraryCount; i++)
    {
        samplers[i] = buildOrbitalSamplerConstexpr(kOrbitalLibrary[i].n, kOrbitalLibrary[i].ell, kOrbitalLibrary[i].m);
    }
    return samplers;
}

// ~12KB per orbital (see main.cpp's original single-sampler note) x 16 orbitals
// here = ~192KB of .rodata, trivial next to this board's 16MB flash. Compiling
// this many constexpr table-builds at once is noticeably slower than the single-
// orbital version but still well inside GCC's default constexpr step/loop limits
// (our loops top out at kOrbitalTableSize=1001 iterations, vs the default
// per-loop limit of 262144) -- if the build times out or hits a limit, the fix
// is -fconstexpr-ops-limit=<bigger>, not shrinking this table.
constexpr std::array<OrbitalSampler, kOrbitalLibraryCount> kOrbitalSamplers = buildOrbitalLibrarySamplers();

/**
 * Look up a library orbital's sampler by (n, ell, m). Runtime O(kOrbitalLibraryCount)
 * linear scan -- fine for a library this small; not meant for a hot path.
 *
 * @return  Pointer to the matching OrbitalSampler, or nullptr if (n,ell,m) isn't
 *          in kOrbitalLibrary.
 */
inline const OrbitalSampler *findOrbitalSampler(int n, int ell, int m)
{
    for (int i = 0; i < kOrbitalLibraryCount; i++)
    {
        if (kOrbitalLibrary[i].n == n && kOrbitalLibrary[i].ell == ell && kOrbitalLibrary[i].m == m)
            return &kOrbitalSamplers[i];
    }
    return nullptr;
}
