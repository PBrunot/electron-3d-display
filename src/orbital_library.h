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

#include "pointcloud.h"

struct OrbitalDescriptor {
    int n, ell, m;
    const char* label;
};

// Index-matched to micropython/cloud_common.py's ORBITAL_PRESETS (same order, same 16
// entries) so preset N here is the same orbital as preset N there -- kept that way on
// purpose for cross-port parity/debugging, not just coincidence. Index 15's (5,2,0) is
// what cloud_common.py itself labels "5p_z3", despite (n=5,ell=2,m=0) actually being a
// 5d_z2 orbital by its own quantum numbers -- ported as-is (same mismatch, not "fixed"
// here) per the parity plan's explicit call on this.
constexpr OrbitalDescriptor kOrbitalLibrary[] = {
    {1, 0, 0, "1s"},       // 0
    {2, 0, 0, "2s"},       // 1
    {2, 1, 1, "2px"},      // 2
    {2, 1, -1, "2py"},     // 3
    {2, 1, 0, "2pz"},      // 4 -- DEFAULT_PRESET_INDEX, matches cloud_common.py
    {3, 0, 0, "3s"},       // 5
    {3, 1, 1, "3px"},      // 6
    {3, 1, -1, "3py"},     // 7
    {3, 1, 0, "3pz"},      // 8
    {3, 2, 0, "3dz2"},     // 9
    {3, 2, 1, "3dxz"},     // 10
    {3, 2, -1, "3dyz"},    // 11
    {3, 2, 2, "3dx2-y2"},  // 12
    {3, 2, -2, "3dxy"},    // 13
    {4, 3, 0, "4fz3"},     // 14
    {5, 2, 0, "5pz3"},     // 15 -- mislabeled in cloud_common.py, see comment above
};
constexpr int kOrbitalLibraryCount = sizeof(kOrbitalLibrary) / sizeof(kOrbitalLibrary[0]);
constexpr int kOrbitalDefaultPresetIndex = 4; // 2pz, matches cloud_common.DEFAULT_PRESET_INDEX

constexpr std::array<OrbitalSampler, kOrbitalLibraryCount> buildOrbitalLibrarySamplers() {
    std::array<OrbitalSampler, kOrbitalLibraryCount> samplers{};
    for (int i = 0; i < kOrbitalLibraryCount; i++) {
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
inline const OrbitalSampler* findOrbitalSampler(int n, int ell, int m) {
    for (int i = 0; i < kOrbitalLibraryCount; i++) {
        if (kOrbitalLibrary[i].n == n && kOrbitalLibrary[i].ell == ell && kOrbitalLibrary[i].m == m)
            return &kOrbitalSamplers[i];
    }
    return nullptr;
}
