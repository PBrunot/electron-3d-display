/**
 * @file orbital_library.h
 * @brief Fixed set of selectable hydrogen orbitals, each with its OrbitalSampler built
 *        entirely at compile time (see pointcloud.h's buildOrbitalSamplerConstexpr()).
 *
 * The real p_x/p_y/p_z and d-orbital shapes fall directly out of psiReal()'s existing
 * convention (cos(m*phi) for m>=0, sin(|m|*phi) for m<0) for the usual m=+-1 (p) / m=+-2 (d)
 * choices -- no separate "real spherical harmonic" math needed, see orbitals.h's psiReal()
 * docstring.
 */
#pragma once

#include <array>
#include <cstdint>

#include "pointcloud.h"

struct OrbitalDescriptor
{
    int n, ell, m;
    const char *label;
    /// Phase-color pair (positive lobe, negative lobe) -- sign of psi_real determines the
    /// color. Every orbital uses the same fixed orange/blue pair so a viewer learns the
    /// sign-color association once and can read it at a glance across the whole library,
    /// rather than each preset having its own distinguishing hue.
    uint16_t posRgb565;
    uint16_t negRgb565;
};

/// Fixed library of orbitals shown by orbital_view.cpp, in menu order.
constexpr OrbitalDescriptor kOrbitalLibrary[] = {
    {1, 0, 0, "1s", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},      // 0
    {2, 0, 0, "2s", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},      // 1
    {2, 1, 1, "2px", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},     // 2
    {2, 1, -1, "2py", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 3
    {2, 1, 0, "2pz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},     // 4 -- kOrbitalDefaultPresetIndex
    {3, 0, 0, "3s", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},      // 5
    {3, 1, 1, "3px", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},     // 6
    {3, 1, -1, "3py", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 7
    {3, 1, 0, "3pz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},     // 8
    {3, 2, 0, "3dz2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 9
    {3, 2, 1, "3dxz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 10
    {3, 2, -1, "3dyz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},   // 11
    {3, 2, 2, "3dx2-y2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue}, // 12
    {3, 2, -2, "3dxy", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},   // 13
    {4, 3, 0, "4fz3", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 14
    {5, 2, 0, "5dz2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 15
};
constexpr int kOrbitalLibraryCount = sizeof(kOrbitalLibrary) / sizeof(kOrbitalLibrary[0]);
constexpr int kOrbitalDefaultPresetIndex = 4; // 2pz

constexpr std::array<OrbitalSampler, kOrbitalLibraryCount> buildOrbitalLibrarySamplers()
{
    std::array<OrbitalSampler, kOrbitalLibraryCount> samplers{};
    for (int i = 0; i < kOrbitalLibraryCount; i++)
    {
        samplers[i] = buildOrbitalSamplerConstexpr(kOrbitalLibrary[i].n, kOrbitalLibrary[i].ell, kOrbitalLibrary[i].m);
    }
    return samplers;
}

/// ~12KB per orbital x 16 orbitals = ~192KB of .rodata, trivial next to this board's 16MB
/// flash. If a build ever times out or hits GCC's constexpr step/loop limit while compiling
/// this table, raise -fconstexpr-ops-limit rather than shrinking the table.
constexpr std::array<OrbitalSampler, kOrbitalLibraryCount> kOrbitalSamplers = buildOrbitalLibrarySamplers();

/**
 * @brief Look up a library orbital's sampler by (n, ell, m).
 * @return Pointer to the matching OrbitalSampler, or nullptr if (n,ell,m) isn't in
 *         kOrbitalLibrary.
 * @note Runtime O(kOrbitalLibraryCount) linear scan -- fine for a library this small; not
 *       meant for a hot path.
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
