// Effective-nuclear-charge model for multi-electron atoms, layered on top of
// orbitals.h's hydrogenic radial wavefunction. Lets the atom point-cloud
// (atom_cloud.h) approximate any element's electron density as a sum of
// hydrogen-LIKE subshells, each shrunk by its own effective nuclear charge
// Z_eff, instead of solving the real many-electron Schrodinger equation.
// Direct C++ port of micropython/slater.py -- see that file's module
// docstring for the full rationale (three stacked textbook approximations:
// Madelung filling with real exceptions, Clementi-Raimondi/Slater Z_eff,
// and Slater's n* rescaling beyond Z=54). Data tables (Clementi-Raimondi
// Z_eff, Madelung exceptions, element symbols) are in slater_data.h,
// mechanically transcribed from micropython/slater_cr_zeff.py and
// micropython/slater.py's _CONFIG_EXCEPTIONS -- do not hand-edit those, see
// that file's header comment.
//
// Header-only (not constexpr): this runs once per element/subshell
// selection (not per frame, not embeddable at compile time since it depends
// on a runtime-chosen Z), so there's nothing to gain from constexpr here --
// unlike orbitals.h/pointcloud.h's table-BUILDING functions.
#pragma once

#include "slater_data.h"

constexpr int kMaxZ = 118;
constexpr int kMaxConfigSubshells = 20; // max observed across Z=1..118 is 19 (see slater_data.h gen)

struct SubshellOcc
{
    int n, ell, occ;
};

struct ElectronConfig
{
    SubshellOcc subshells[kMaxConfigSubshells];
    int count = 0;
};

// (n+ell, n) Madelung order, ell<=3 (s/p/d/f), n<=8 -- sufficient for every
// element's ground-state configuration up to kMaxZ=118 under this simplified
// rule (the g subshell, ell=4, is never populated in n+l order until past
// Z=120). Hardcoded from micropython/slater.py's _aufbau_order() output
// rather than re-sorted here, to avoid depending on a runtime sort for a
// fixed, tiny, well-known sequence.
constexpr int kAufbauOrder[26][2] = {
    {1, 0},
    {2, 0},
    {2, 1},
    {3, 0},
    {3, 1},
    {4, 0},
    {3, 2},
    {4, 1},
    {5, 0},
    {4, 2},
    {5, 1},
    {6, 0},
    {4, 3},
    {5, 2},
    {6, 1},
    {7, 0},
    {5, 3},
    {6, 2},
    {7, 1},
    {8, 0},
    {6, 3},
    {7, 2},
    {8, 1},
    {7, 3},
    {8, 2},
    {8, 3},
};

constexpr int subshellCapacity(int ell)
{
    return 2 * (2 * ell + 1);
}

/**
 * Ground-state electron configuration for atomic number z: the simple
 * Madelung (n+l) filling rule, with the real-world exceptions in
 * slater_data.h's kConfigExceptions applied. Port of
 * slater.electron_configuration().
 *
 * @param z  Atomic number, 1 <= z <= kMaxZ.
 * @return   Subshells in Madelung fill order for regular elements, or
 *           n-then-ell chemistry order for the exceptions (matching the
 *           source table).
 */
inline ElectronConfig electronConfiguration(int z)
{
    ElectronConfig config{};

    for (int i = 0; i < kExceptionZsCount; i++)
    {
        if (kExceptionZs[i] == z)
        {
            for (int j = 0; j < kConfigExceptionsCount; j++)
            {
                if (kConfigExceptions[j].z == z)
                {
                    config.subshells[config.count++] = {kConfigExceptions[j].n, kConfigExceptions[j].ell,
                                                        kConfigExceptions[j].occ};
                }
            }
            return config;
        }
    }

    int remaining = z;
    for (int i = 0; i < 26 && remaining > 0; i++)
    {
        int n = kAufbauOrder[i][0];
        int ell = kAufbauOrder[i][1];
        int occ = remaining < subshellCapacity(ell) ? remaining : subshellCapacity(ell);
        config.subshells[config.count++] = {n, ell, occ};
        remaining -= occ;
    }
    return config;
}

inline char subshellLabelChar(int ell)
{
    static const char kLabels[] = "spdf";
    return (ell >= 0 && ell <= 3) ? kLabels[ell] : '?';
}

/**
 * Slater's conventional group key comparison: (1s) / (2s,2p) / (3s,3p) /
 * (3d) / (4s,4p) / (4d) / (4f) / ... -- s and p subshells of the same n are
 * one group; d and f subshells are each their own group. Port of
 * slater._slater_group(), returned as a single comparable rank instead of a
 * tuple: (n, ell<=1?0:ell) packed as n*10 + (ell<=1?0:ell), safe since
 * ell<=3 and n<=8 here.
 */
constexpr int slaterGroupRank(int n, int ell)
{
    return n * 10 + (ell <= 1 ? 0 : ell);
}

/**
 * Effective nuclear charge seen by ONE electron in subshell (n, ell), via
 * Slater's rules, given the full ground-state `config` for atomic number
 * `z`. Port of slater.slater_z_eff() -- see that function's docstring for
 * the shielding-constant rules.
 */
inline orb_real_t slaterZEff(int z, const ElectronConfig &config, int n, int ell)
{
    int targetGroup = slaterGroupRank(n, ell);
    int sameGroupTotal = 0;
    orb_real_t shield = orb_real_t(0);

    for (int i = 0; i < config.count; i++)
    {
        int cn = config.subshells[i].n, cell = config.subshells[i].ell, occ = config.subshells[i].occ;
        if (slaterGroupRank(cn, cell) == targetGroup)
        {
            sameGroupTotal += occ;
            continue;
        }
        if (ell <= 1)
        {
            if (cn == n - 1)
                shield += orb_real_t(0.85) * orb_real_t(occ);
            else if (cn < n - 1)
                shield += orb_real_t(1.00) * orb_real_t(occ);
        }
        else
        {
            if (cn < n || (cn == n && cell < ell))
                shield += orb_real_t(1.00) * orb_real_t(occ);
        }
    }

    orb_real_t sameGroupFactor = (n == 1 && ell == 0) ? orb_real_t(0.30) : orb_real_t(0.35);
    int extra = sameGroupTotal - 1;
    if (extra > 0)
        shield += sameGroupFactor * orb_real_t(extra);

    orb_real_t result = orb_real_t(z) - shield;
    return result > orb_real_t(1) ? result : orb_real_t(1);
}

/**
 * Slater's effective principal quantum number n* (Slater, Phys. Rev. 36, 57
 * (1930)): 1,2,3,3.7,4.0,4.2 for n=1..6; n>=7 reuses 4.2. Port of
 * slater.n_star().
 */
inline orb_real_t nStar(int n)
{
    static const orb_real_t kNStar[] = {orb_real_t(1), orb_real_t(2), orb_real_t(3),
                                        orb_real_t(3.7), orb_real_t(4.0), orb_real_t(4.2)};
    if (n <= 0)
        return kNStar[0];
    if (n <= 6)
        return kNStar[n - 1];
    return kNStar[5];
}

/**
 * Clementi-Raimondi Z_eff for subshell (n, ell) of element z, from
 * slater_data.h's kCrZEff (Z <= 54 only). Port of slater.z_eff_cr().
 *
 * @return  true and *outZeff set if the table covers (z, n, ell); false
 *          otherwise (caller should fall back to slaterZEff()).
 */
inline bool zEffCr(int z, int n, int ell, orb_real_t *outZeff)
{
    for (int i = 0; i < kCrZEffCount; i++)
    {
        if (kCrZEff[i].z == z && kCrZEff[i].n == n && kCrZEff[i].ell == ell)
        {
            *outZeff = orb_real_t(kCrZEff[i].zeff);
            return true;
        }
    }
    return false;
}

/**
 * Effective charge to use in the r -> Z_eff*r substitution of the
 * hydrogenic radial wavefunction for subshell (n, ell) of element z. Port
 * of slater.z_eff_radial() -- Clementi-Raimondi where available (Z<=54,
 * used as-is), else Slater's Z_eff rescaled by n/n* (see that function's
 * docstring for why).
 */
inline orb_real_t zEffRadial(int z, const ElectronConfig &config, int n, int ell)
{
    orb_real_t cr;
    if (zEffCr(z, n, ell, &cr))
        return cr;
    return slaterZEff(z, config, n, ell) * orb_real_t(n) / nStar(n);
}

struct MOcc
{
    int m, occ;
};

/**
 * Electron occupancy per individual real orbital (magnetic quantum number
 * m, -ell<=m<=ell) for `occ` electrons in an ell subshell, via Hund's rule:
 * one electron into each of the 2*ell+1 orbitals first (in m order), then a
 * second into each in the same order. Port of slater.hund_fill_m().
 *
 * @param out    [out] Receives occupied (m, occupancy) pairs, occupancy in
 *               {1,2}; must hold at least 2*ell+1 entries.
 * @return       Number of entries written to out (only occupied m's).
 */
inline int hundFillM(int ell, int occ, MOcc *out)
{
    int slots = 2 * ell + 1;
    int occM[2 * kOrbitalEllMax + 1] = {};

    int remaining = occ;
    int i = 0;
    while (remaining > 0 && i < slots)
    {
        occM[i] = 1;
        remaining--;
        i++;
    }
    i = 0;
    while (remaining > 0)
    {
        occM[i] = 2;
        remaining--;
        i++;
    }

    int count = 0;
    for (int k = 0; k < slots; k++)
    {
        if (occM[k] > 0)
        {
            out[count].m = k - ell;
            out[count].occ = occM[k];
            count++;
        }
    }
    return count;
}

/// @brief return the element symbol for atomic number z (1-based, 1=H, 2=He, ..., 118=Og)
/// @param z Atomic number, 1 <= z <= kMaxZ
/// @return char pointer to the element symbol string (e.g. "H", "He", "Li", ..., "Og")
inline const char *elementSymbol(int z)
{
    return kElementSymbols[z - 1];
}
