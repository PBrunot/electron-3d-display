/**
 * @file orbital_library.h
 * @brief Fixed set of selectable hydrogen orbitals (kOrbitalLibrary), whose OrbitalSampler
 *        tables are read ON DEMAND from flash (src/orbital_library.cpp) instead of compiled
 *        into firmware -- see that file's header comment for the loading strategy, and
 *        tools/orbital_table_gen.py for how data/orbital_samplers.bin is produced.
 *
 * The real p_x/p_y/p_z and d-orbital shapes fall directly out of psiReal()'s existing
 * convention (cos(m*phi) for m>=0, sin(|m|*phi) for m<0) for the usual m=+-1 (p) / m=+-2 (d)
 * choices -- no separate "real spherical harmonic" math needed, see orbitals.h's psiReal()
 * docstring.
 */
#pragma once

#include <cstdint>

#include "display.h" // Display::kColorOrbitalRed/kColorOrbitalBlue, used by kOrbitalLibrary below
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
inline constexpr OrbitalDescriptor kOrbitalLibrary[] = {
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
    // Rest of n=4 (n=1..3 were already a complete set above; index-matched to
    // cloud_common.py's ORBITAL_PRESETS -- keep both lists appended in the same order.
    {4, 0, 0, "4s", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},            // 16
    {4, 1, 1, "4px", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},           // 17
    {4, 1, -1, "4py", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},          // 18
    {4, 1, 0, "4pz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},           // 19
    {4, 2, 0, "4dz2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},          // 20
    {4, 2, 1, "4dxz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},          // 21
    {4, 2, -1, "4dyz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},         // 22
    {4, 2, 2, "4dx2-y2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},       // 23
    {4, 2, -2, "4dxy", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},         // 24
    {4, 3, 1, "4fxz2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},         // 25
    {4, 3, -1, "4fyz2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},        // 26
    {4, 3, 2, "4fzx2-y2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},      // 27
    {4, 3, -2, "4fxyz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},        // 28
    {4, 3, 3, "4fxx2-3y2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},     // 29
    {4, 3, -3, "4fy3x2-y2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},    // 30
    // Selected n=5/n=6 orbitals (not the full set -- n=5/6 add far more (n,l,m)
    // combinations than n<=4; these are the specific ones requested).
    {5, 1, 0, "5pz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},           // 31
    {5, 2, 1, "5dxz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},          // 32
    {5, 2, 2, "5dx2-y2", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},       // 33
    {6, 0, 0, "6s", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},            // 34
    {6, 1, 0, "6pz", Display::kColorOrbitalRed, Display::kColorOrbitalBlue},           // 35
};
inline constexpr int kOrbitalLibraryCount = sizeof(kOrbitalLibrary) / sizeof(kOrbitalLibrary[0]);
inline constexpr int kOrbitalDefaultPresetIndex = 4; // 2pz

/**
 * @brief Look up a library orbital's sampler by (n, ell, m), reading it on demand from
 *        data/orbital_samplers.bin (see orbital_library.cpp).
 *
 * @return Pointer to a sampler for (n, ell, m) -- valid ONLY until the next
 *         findOrbitalSampler() call (single shared static instance, same convention as
 *         hfs_radial.h's hfsFindU()), fine since only one preset is ever active at a time
 *         (orbital_view.h's OrbitalPresetState) and every caller consumes the pointer before
 *         the next preset switch. nullptr only if (n,ell,m) isn't in kOrbitalLibrary at all
 *         (a caller bug -- every real call site passes n/ell/m straight from a
 *         kOrbitalLibrary entry, so this shouldn't happen in practice). If the entry IS in
 *         the library but data/orbital_samplers.bin hasn't been deployed yet (`pio run -t
 *         uploadfs`) or fails to read, still returns a valid pointer to a degenerate
 *         (all-zero -> single point at the origin) sampler rather than nullptr or a crash --
 *         see orbital_library.cpp's own comment for why there's no better fallback available
 *         here (unlike atom_cloud.h's hydrogenic fallback for the optional HFS table,
 *         hydrogen orbitals ARE this project's base model, nothing to fall back to).
 * @note Runtime O(kOrbitalLibraryCount) linear scan for the index, then one flash read --
 *       fine for a library this small, called once per preset switch, not once per frame or
 *       per sampled point.
 */
const OrbitalSampler *findOrbitalSampler(int n, int ell, int m);
