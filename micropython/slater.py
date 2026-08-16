"""Effective-nuclear-charge model for multi-electron atoms, layered on top
of orbitals.py's hydrogenic radial wavefunction. Lets atom_cloud.py
approximate any element's electron density as a sum of hydrogen-LIKE
subshells, each shrunk by its own effective nuclear charge Z_eff, instead
of solving the real (much harder) many-electron Schrodinger equation. Pure
data/math, no display/hardware imports -- shared between the PC simulator
and (eventually) the device, same as orbitals.py/pointcloud.py.

Three standard textbook approximations stacked here (all explicit, none
specific to this project):

  1. Electron configuration is filled by the simple n+l (Madelung) rule,
     with the well-known real exceptions (Cr, Cu, Nb, Mo, Ru, Rh, Pd, Ag,
     Pt, Au and the La/Ce/Gd/Ac/Th/Pa/U/Np/Cm/Lr anomalies) overridden by
     an explicit table (see _CONFIG_EXCEPTIONS). For elements like Pd
     (4d10, no 5s) the Madelung rule otherwise puts an electron in the
     WRONG, much more diffuse outermost subshell.

  2. Z_eff comes from the refined Hartree-Fock values of Clementi,
     Raimondi & Reinhardt (1963/1967, see slater_cr_zeff.py) wherever the
     table covers the subshell (Z <= 54), falling back to Slater's rules
     (Slater, Phys. Rev. 36, 57 (1930)) beyond that -- Slater's constants
     are documented as progressively less accurate for heavier elements.

  3. For subshells that DO fall back to Slater's rules, the effective
     charge used in the hydrogenic radial substitution is additionally
     rescaled by n/n* (Slater's effective principal quantum number, see
     n_star()): Slater's Z_eff was calibrated for STO exponents Z_eff/n*,
     so using it with the true hydrogenic exponent Z_eff/n overestimates
     radii by n/n* for n >= 4 (8% at n=4, 25% at n=5, 43% at n=6). CR
     Z_eff values are NOT rescaled: they are defined via the actual n
     (Z_eff = n*sqrt(-2E)), so they are n-consistent by construction.

Good enough to get shell contraction right (Z_eff grows across a period,
inner shells sit at higher effective Z than outer ones) for a visual demo --
not meant to reproduce spectroscopic-grade energies.
"""

MAX_Z = 118

from slater_cr_zeff import CR_Z_EFF  # noqa: E402 -- data module, same package

_SUBSHELL_CAPACITY = {0: 2, 1: 6, 2: 10, 3: 14}
_SUBSHELL_LABELS = 'spdf'


def _aufbau_order(max_n=8, max_ell=3):
    """(n, ell) pairs in Madelung (n+l, then n) filling order, restricted to
    ell<=max_ell=3 (s/p/d/f) -- sufficient for every element's ground-state
    configuration up to MAX_Z=118 under this simplified rule; the g subshell
    (ell=4) is never populated in n+l order until past Z=120.
    """
    subshells = [(n, ell) for n in range(1, max_n + 1) for ell in range(0, min(max_ell, n - 1) + 1)]
    subshells.sort(key=lambda t: (t[0] + t[1], t[0]))
    return subshells


_AUFBAU_ORDER = _aufbau_order()


# Real ground-state configurations that deviate from the n+l (Madelung)
# filling rule, in the standard chemistry order (n then ell) -- same
# (n, ell, occupancy) triple format as electron_configuration()'s output.
# Sources: NIST ground-state levels / standard periodic-table data; the
# entries are the well-established s->d (Cr, Cu, Nb, Mo, Ru, Rh, Pd, Ag,
# Pt, Au) and f/d (La, Ce, Gd, Ac, Th, Pa, U, Np, Cm, Lr) anomalies. Each
# entry is the FULL configuration so the list is self-contained.
_CONFIG_EXCEPTIONS = {
    24: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 5), (4, 0, 1)],       # Cr 3d5 4s1
    29: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 1)],     # Cu 3d10 4s1
    41: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 4), (5, 0, 1)],    # Nb 4d4 5s1
    42: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 5), (5, 0, 1)],    # Mo 4d5 5s1
    44: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 7), (5, 0, 1)],    # Ru 4d7 5s1
    45: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 8), (5, 0, 1)],    # Rh 4d8 5s1
    46: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10)],              # Pd 4d10 (no 5s)
    47: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (5, 0, 1)],  # Ag 4d10 5s1
    57: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (5, 0, 2), (5, 1, 6), (5, 2, 1), (6, 0, 2)],                    # La 5d1 6s2
    58: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 1), (5, 0, 2), (5, 1, 6), (5, 2, 1), (6, 0, 2)],       # Ce 4f1 5d1 6s2
    64: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 7), (5, 0, 2), (5, 1, 6), (5, 2, 1), (6, 0, 2)],        # Gd 4f7 5d1 6s2
    78: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 9), (6, 0, 1)],      # Pt 5d9 6s1
    79: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (6, 0, 1)],     # Au 5d10 6s1
    89: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (6, 0, 2), (6, 1, 6), (6, 2, 1), (7, 0, 2)],  # Ac 6d1 7s2
    90: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (6, 0, 2), (6, 1, 6), (6, 2, 2), (7, 0, 2)],  # Th 6d2 7s2
    91: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (5, 3, 2), (6, 0, 2), (6, 1, 6), (6, 2, 1), (7, 0, 2)],   # Pa 5f2 6d1 7s2
    92: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (5, 3, 3), (6, 0, 2), (6, 1, 6), (6, 2, 1), (7, 0, 2)],   # U 5f3 6d1 7s2
    93: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (5, 3, 4), (6, 0, 2), (6, 1, 6), (6, 2, 1), (7, 0, 2)],  # Np 5f4 6d1 7s2
    96: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (5, 3, 7), (6, 0, 2), (6, 1, 6), (6, 2, 1), (7, 0, 2)],  # Cm 5f7 6d1 7s2
    103: [(1, 0, 2), (2, 0, 2), (2, 1, 6), (3, 0, 2), (3, 1, 6), (3, 2, 10), (4, 0, 2), (4, 1, 6), (4, 2, 10), (4, 3, 14), (5, 0, 2), (5, 1, 6), (5, 2, 10), (5, 3, 14), (6, 0, 2), (6, 1, 6), (7, 0, 2), (7, 1, 1)],  # Lr 5f14 7s2 7p1
}


def electron_configuration(z):
    """Ground-state electron configuration for atomic number z -- the
    simple Madelung (n+l) filling rule, with the real-world exceptions in
    _CONFIG_EXCEPTIONS applied (see module docstring).

    Returns a list of (n, ell, occupancy) triples: Madelung fill order for
    regular elements, n-then-ell chemistry order for the exceptions.
    """
    if not (1 <= z <= MAX_Z):
        raise ValueError("z must be within 1..%d, got %d" % (MAX_Z, z))
    if z in _CONFIG_EXCEPTIONS:
        return list(_CONFIG_EXCEPTIONS[z])
    remaining = z
    config = []
    for n, ell in _AUFBAU_ORDER:
        if remaining <= 0:
            break
        occ = min(_SUBSHELL_CAPACITY[ell], remaining)
        config.append((n, ell, occ))
        remaining -= occ
    return config


def subshell_label(n, ell):
    return "%d%s" % (n, _SUBSHELL_LABELS[ell])


def configuration_str(config):
    """e.g. [(1,0,2),(2,0,2),(2,1,2)] -> '1s2 2s2 2p2'."""
    return " ".join("%s%d" % (subshell_label(n, ell), occ) for n, ell, occ in config)


def _slater_group(n, ell):
    """Slater's conventional group key: (1s) / (2s,2p) / (3s,3p) / (3d) /
    (4s,4p) / (4d) / (4f) / ... -- s and p subshells of the same n are one
    group; d and f subshells are each their own group. Comparing these
    tuples reproduces the left-to-right order Slater's rules are stated in
    (grouped by n first, then s/p < d < f within that n) because ell 0/1
    both map to rank 0 while d=2, f=3 sort strictly after.
    """
    return (n, 0) if ell <= 1 else (n, ell)


def slater_z_eff(z, config, n, ell):
    """Effective nuclear charge seen by ONE electron in subshell (n, ell),
    via Slater's rules, given the full ground-state `config` (as returned by
    electron_configuration()) for atomic number `z`.

    Shielding constant S is built from every OTHER electron in `config`:
      - same group (see _slater_group): 0.35 each (0.30 instead, only for
        the 1s group -- Slater's original special case).
      - querying an s/p electron (ell<=1): shell n-1 shields 0.85 each,
        shells below n-1 shield 1.00 each (shell n or above, other than the
        same group: 0 -- outer electrons don't shield inner ones).
      - querying a d/f electron (ell>=2): any electron in a group that
        comes before this one in Slater's group order shields 1.00 each
        (electrons in later groups: 0).

    Returns Z - S, floored at 1.0 (a bound electron seeing zero or negative
    effective charge is unphysical; the floor only matters for
    pathological/very-heavy-atom edge cases, not any real element modeled
    here).
    """
    target_group = _slater_group(n, ell)
    same_group_total = 0
    shield = 0.0
    for cn, cell, occ in config:
        if _slater_group(cn, cell) == target_group:
            same_group_total += occ
            continue
        if ell <= 1:
            if cn == n - 1:
                shield += 0.85 * occ
            elif cn < n - 1:
                shield += 1.00 * occ
        else:
            if cn < n or (cn == n and cell < ell):
                shield += 1.00 * occ

    same_group_factor = 0.30 if (n == 1 and ell == 0) else 0.35
    shield += same_group_factor * max(same_group_total - 1, 0)

    return max(z - shield, 1.0)


# Slater's effective principal quantum number n*, indexed by n (index 0 unused).
# From Slater (1930): n=1..3 -> n, n=4 -> 3.7, n=5 -> 4.0, n=6 -> 4.2. Slater's own
# table stops at n=6; the last value is reused for n >= 7 (a documented extension,
# needed for the 7s/7p shells of the actinides -- Fr, Ra, ...).
_N_STAR = (None, 1.0, 2.0, 3.0, 3.7, 4.0, 4.2)


def n_star(n):
    """Slater's effective principal quantum number n* for principal quantum
    number n (Slater, Phys. Rev. 36, 57 (1930)): 1, 2, 3, 3.7, 4.0, 4.2 for
    n = 1..6; n >= 7 reuses 4.2 (extension, see _N_STAR). n* is the quantum
    number Slater's rules were calibrated with: his Z_eff values are meant for
    Slater-type-orbital exponents Z_eff/n*, so using them in the true hydrogenic
    exponent Z_eff/n overestimates radii by n/n* for n >= 4 (see z_eff_radial()).
    """
    return _N_STAR[n] if n < len(_N_STAR) else _N_STAR[-1]


def z_eff_cr(z, config, n, ell):
    """Clementi-Raimondi Z_eff for subshell (n, ell) of element z, from the
    Hartree-Fock SCF table in slater_cr_zeff.py (Clementi & Raimondi 1963,
    Clementi, Raimondi & Reinhardt 1967). Returns None when the table does not
    cover this element/subshell (Z > 54, or a subshell absent from the table
    because it is unoccupied in the real ground state -- e.g. Pd 5s). `config`
    is accepted for signature symmetry with slater_z_eff(); it is not used.
    """
    return CR_Z_EFF.get(z, {}).get((n, ell))


def z_eff(z, config, n, ell):
    """Effective nuclear charge for a subshell (n, ell) of element z: the refined
    Clementi-Raimondi Hartree-Fock value where the table covers it, else
    Slater's rules. This is the raw Z_eff (no n* rescaling) -- use
    z_eff_radial() for the value that actually goes into the hydrogenic radial
    substitution.
    """
    cr = z_eff_cr(z, config, n, ell)
    if cr is not None:
        return cr
    return slater_z_eff(z, config, n, ell)


def z_eff_radial(z, config, n, ell):
    """Effective charge to use in the r -> Z_eff*r substitution of the hydrogenic
    radial wavefunction for subshell (n, ell) of element z -- the single number
    atom_cloud.py passes to pointcloud's samplers (and to psi_real() for the sign
    recomputation, which must use the same substitution).

    - Clementi-Raimondi Z_eff (where available, Z <= 54): used as-is. CR Z_eff
      is defined via the actual principal quantum number (Z_eff = n*sqrt(-2E)),
      so it is n-consistent with the hydrogenic exponent Z_eff/n by construction.
    - Slater's Z_eff (fallback, Z > 54): rescaled by n/n* (n_star()). Slater's
      constants were calibrated for STO exponents Z_eff/n*, so using them with
      the hydrogenic exponent Z_eff/n makes radii systematically too large by
      n/n* (8% at n=4, 25% at n=5, 43% at n=6); the rescaling contracts the
      radial coordinate by n*/n to restore Slater's intended sizes.
    """
    cr = z_eff_cr(z, config, n, ell)
    if cr is not None:
        return cr
    return slater_z_eff(z, config, n, ell) * n / n_star(n)


def hund_fill_m(ell, occ):
    """Electron occupancy per individual real orbital (magnetic quantum
    number m, -ell<=m<=ell) for `occ` electrons in an ell subshell, via
    Hund's rule: one electron (spin unpaired) into each of the 2*ell+1
    orbitals first, in m order, THEN a second (paired) electron into each
    in the same order once every orbital already has one. This is the
    standard ground-state filling rule, and it's what gives a
    partially-filled subshell its real (non-spherical) shape: p1/p2/p4/p5
    only populate some of the 3 p orbitals (some doubly), so the combined
    density is anisotropic; only a FULL or (coincidentally) exactly
    half-filled-with-every-orbital-singly-occupied subshell is isotropic.

    Which physical direction each m actually points is a labeling
    convention (see cloud_common.ORBITAL_PRESETS's p_x/p_y/p_z docstring
    comment) -- Hund's rule itself only fixes HOW MANY orbitals are
    singly/doubly occupied, not which spatial axis each one lands on, so
    the m order used here is arbitrary but fixed (consistent from one call
    to the next, which is all atom_cloud.py needs).

    Returns a list of (m, occupancy) pairs, occupancy in {1, 2}, for
    occupied m's only (occ=0 -> empty list).
    """
    m_values = list(range(-ell, ell + 1))
    slots = len(m_values)
    if not (0 <= occ <= 2 * slots):
        raise ValueError("occ must be within 0..%d for ell=%d, got %d" % (2 * slots, ell, occ))

    occ_m = [0] * slots
    remaining = occ
    i = 0
    while remaining > 0 and i < slots:
        occ_m[i] = 1
        remaining -= 1
        i += 1
    i = 0
    while remaining > 0:
        occ_m[i] = 2
        remaining -= 1
        i += 1

    return [(m_values[i], occ_m[i]) for i in range(slots) if occ_m[i] > 0]


_ELEMENT_SYMBOLS = (
    "H He Li Be B C N O F Ne Na Mg Al Si P S Cl Ar K Ca Sc Ti V Cr Mn Fe Co Ni Cu Zn Ga Ge As Se Br Kr "
    "Rb Sr Y Zr Nb Mo Tc Ru Rh Pd Ag Cd In Sn Sb Te I Xe Cs Ba La Ce Pr Nd Pm Sm Eu Gd Tb Dy Ho Er Tm Yb Lu "
    "Hf Ta W Re Os Ir Pt Au Hg Tl Pb Bi Po At Rn Fr Ra Ac Th Pa U Np Pu Am Cm Bk Cf Es Fm Md No Lr "
    "Rf Db Sg Bh Hs Mt Ds Rg Cn Nh Fl Mc Lv Ts Og"
).split()

assert len(_ELEMENT_SYMBOLS) == MAX_Z


def element_symbol(z):
    return _ELEMENT_SYMBOLS[z - 1]
