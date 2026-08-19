// Every (ell, m) angular-table combination needed for s/p/d/f subshells (ell<=3),
// built entirely at COMPILE time (see pointcloud.h's buildAngularTablesConstexpr() and
// the block comment above it for why angular and radial tables are split for the
// multi-element atom viewer). Angular tables are Z_eff- AND n-independent, so this
// library covers every element/subshell in slater.h's Madelung order -- only the much
// smaller per-(n,ell,Z_eff) radial table needs to be built at runtime per element
// (atom_cloud.h).
#pragma once

#include <array>

#include "pointcloud.h"

struct AngularDescriptor {
    int ell, m;
};

inline constexpr AngularDescriptor kAngularLibraryDescriptors[] = {
    {0, 0},
    {1, -1}, {1, 0}, {1, 1},
    {2, -2}, {2, -1}, {2, 0}, {2, 1}, {2, 2},
    {3, -3}, {3, -2}, {3, -1}, {3, 0}, {3, 1}, {3, 2}, {3, 3},
};
inline constexpr int kAngularLibraryCount = sizeof(kAngularLibraryDescriptors) / sizeof(kAngularLibraryDescriptors[0]);

constexpr std::array<OrbitalAngularTables, kAngularLibraryCount> buildAngularLibrary() {
    std::array<OrbitalAngularTables, kAngularLibraryCount> tables{};
    for (int i = 0; i < kAngularLibraryCount; i++) {
        tables[i] = buildAngularTablesConstexpr(kAngularLibraryDescriptors[i].ell, kAngularLibraryDescriptors[i].m);
    }
    return tables;
}

// ~8KB/entry (2 x 1001-float tables) x 16 entries =~ 128KB of .rodata.
inline constexpr std::array<OrbitalAngularTables, kAngularLibraryCount> kAngularLibrary = buildAngularLibrary();

/**
 * Look up the angular tables for (ell, m). Runtime O(kAngularLibraryCount) linear scan --
 * fine for a library this small (16 entries), not meant for a hot path.
 *
 * @return  Pointer to the matching OrbitalAngularTables, or nullptr if (ell,m) isn't in
 *          kAngularLibraryDescriptors (shouldn't happen for any ell<=3, |m|<=ell pair).
 */
inline const OrbitalAngularTables* findAngularTables(int ell, int m) {
    for (int i = 0; i < kAngularLibraryCount; i++) {
        if (kAngularLibraryDescriptors[i].ell == ell && kAngularLibraryDescriptors[i].m == m)
            return &kAngularLibrary[i];
    }
    return nullptr;
}
