"""Slater's-rules effective-nuclear-charge model for multi-electron atoms,
layered on top of orbitals.py's hydrogenic radial wavefunction. Lets
atom_cloud.py approximate any element's electron density as a sum of
hydrogen-LIKE subshells, each shrunk by its own effective nuclear charge
Z_eff, instead of solving the real (much harder) many-electron Schrodinger
equation. Pure data/math, no display/hardware imports -- shared between the
PC simulator and (eventually) the device, same as orbitals.py/pointcloud.py.

Two approximations stacked here, both standard textbook simplifications,
not something specific to this project:
  1. Electron configuration is filled by the simple n+l (Madelung) rule --
     the handful of real exceptions (Cr, Cu, Nb, ... where a d subshell
     "steals" an electron from the ns subshell for extra stability) are
     ignored.
  2. Z_eff is estimated per SUBSHELL via Slater's rules (Slater, Phys. Rev.
     36, 57 (1930)), not solved self-consistently -- real Z_eff varies
     slightly per electron within a subshell too, and Slater's rules are
     themselves a fit to more accurate calculations, not exact.
Good enough to get shell contraction right (Z_eff grows across a period,
inner shells sit at higher effective Z than outer ones) for a visual demo --
not meant to reproduce spectroscopic-grade energies.
"""

MAX_Z = 118

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


def electron_configuration(z):
    """Ground-state electron configuration for atomic number `z` via the
    simple Madelung (n+l) filling rule -- see module docstring for the
    known real-world exceptions this ignores.

    Returns a list of (n, ell, occupancy) triples in fill order.
    """
    if not (1 <= z <= MAX_Z):
        raise ValueError("z must be within 1..%d, got %d" % (MAX_Z, z))
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
