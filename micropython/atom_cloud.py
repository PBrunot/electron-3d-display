"""Multi-electron atom point-cloud approximation, built on top of
orbitals.py/pointcloud.py's hydrogenic math and slater.py's effective-
nuclear-charge model. PC-only for now (see pc/atom_view_pc.py) -- not yet
wired into the ESP32 firmware path.

Model, in one paragraph: for atomic number Z, fill subshells by the Madelung
rule with the known real-world exceptions applied
(slater.electron_configuration() / slater._CONFIG_EXCEPTIONS), give every
electron in a subshell the SAME effective nuclear charge Z_eff used in the
radial substitution r -> Z_eff*r (slater.z_eff_radial(): the refined
Clementi-Raimondi Hartree-Fock Z_eff where the table covers the subshell,
else Slater's rules rescaled by n/n* -- Slater's n* consistency; one value
per subshell, not per electron). A FULL subshell's contribution is sampled
as spherically symmetric (pointcloud.sample_isotropic_point()) -- exact via
Unsoeld's theorem (summing |Y_l^m|^2 over every m in a filled subshell gives
a constant). A subshell that is NOT full is instead expanded into its
individually-occupied real orbitals per Hund's rule
(slater.hund_fill_m()) and each sampled with the SAME per-orbital sampler
the hydrogen presets use (pointcloud.sample_orbital_point(), Z_eff-scaled --
see pointcloud.init_orbital_sampler()'s z_eff parameter) -- this is what
gives a partially-filled outer shell (e.g. carbon's 2p2) its real lobed
shape instead of a featureless sphere; treating it as isotropic too, like an
earlier version of this module did, is only exact for full subshells. The
atom's total point cloud is the union of every group's own point cloud,
point count split proportional to how many electrons that group represents
-- EXCEPT that anisotropic (partially-filled, m is not None) groups get
their share multiplied by PARTIAL_SUBSHELL_BOOST first. Without the boost,
the lobed shape is real but visually weak for most elements: filled s/p/d
subshells are exactly isotropic (not an approximation -- l=0 has no angular
dependence at all, and Unsoeld's theorem makes any FULL subshell isotropic
too) and usually outnumber the partially-filled one, and for elements like
carbon the filled inner subshell (2s) even reaches slightly FARTHER out on
average than the lobed one (2p) -- <r> ~ n^2*(3 - l(l+1)/n^2), 6 vs 5 in
units of a0/Z_eff for n=2 -- so the isotropic background dominates exactly
where the eye is looking, at the cloud's outer edge. The boost trades exact
per-electron point density for a shape that actually reads as lobed on a
240x240 display; it does not change WHICH points are anisotropic, only how
many of the total budget go to that group.

Coloring: every point's default color (SHELL_RGB) is still by shell
(principal quantum number n) regardless of which path produced it, so the
classic K/L/M/N shell structure reads visually across the whole atom. Every
point ALSO carries a signed-wavefunction sign (see `signs`, below) wherever
one is meaningful: isotropic (full-subshell) groups have none -- angle-
averaging erases it, same reasoning Unsoeld's theorem relies on above -- and
are marked sign=0; anisotropic (Hund's-rule per-orbital) groups DO have a
real signed wavefunction, recovered here by evaluating orbitals.psi_real()
at each sampled point (same z_eff*r substitution the radial table itself
was built with, see init_orbital_sampler()'s docstring) after the fact --
the inverse-CDF sampling that placed the point only ever used |psi|^2,
which loses the sign, so it has to be recomputed, not reused. Not applied
to `colors` itself (still SHELL_RGB everywhere, unconditionally) since
mixing signed/unsigned coloring across one atom's cloud by default would be
more confusing than informative; instead left for a caller that wants it
for a specific purpose to use `signs` selectively -- see
pc/atom_view_pc.py's shell-dissection view, which swaps to phase coloring
(cloud_common.PHASE_POSITIVE_RGB/PHASE_NEGATIVE_RGB) only for the ONE shell
currently exploded, where "isotropic groups have none" doesn't collide with
anything else on screen -- and only for its anisotropic points; sign=0
points there keep reading as their normal shell color.

No point-turnover/resample here either (unlike cloud_common.ResampleState)
-- the cloud is built once and stays static; see pc/atom_view_pc.py's
module docstring for that tradeoff.
"""

import array
import math

import cloud_common
import orbitals
import pointcloud
import slater

N_POINTS = 10000  # PC default; matches pc/orbital_view_pc.py's hydrogen-preset count
SEED = 12345

# Multiplier applied to a partially-filled (anisotropic) group's weight
# before splitting the point budget across groups -- see module docstring's
# "EXCEPT" paragraph for why the un-boosted (exactly-per-electron) split
# reads as nearly spherical even for a genuinely lobed configuration like
# carbon's 2p2. Chosen empirically (3x roughly doubles carbon's on-axis vs
# off-axis point-density ratio measured via a 20deg cone test -- see the
# conversation history / commit message this constant was introduced in --
# without starving the isotropic groups down to visible sparseness). Applied
# uniformly to every anisotropic group regardless of element; a full
# subshell's groups (m is None) are never boosted.
PARTIAL_SUBSHELL_BOOST = 3.0

# 1 Bohr radius in Angstrom (CODATA a0 = 0.52917721090(80)e-10 m, rounded to
# float precision) -- lets pc/atom_view_pc.py's scale bar report physical
# size in the unit chemists actually use, since every length elsewhere in
# this module (and in orbitals.py/pointcloud.py) is implicitly in Bohr
# radii (see orbitals.py's module docstring: r is physical radius with
# a0=1, Z=1 folded into the hydrogenic formula).
ANGSTROM_PER_BOHR = 0.529177210903

# Calibration for scale_for_atom() below: reference atomic number and its
# on-screen target size, used ONCE at import time to derive PIXELS_PER_BOHR
# (a single pixels-per-Bohr-radius conversion factor shared by every
# element -- see scale_for_atom()'s docstring for why this must NOT be
# cloud_common.scale_from_radii()'s per-cloud renormalization).
#
# Lithium (Z=3) is the reference because it's the largest atom this model
# produces across the whole Z=1..118 range it supports (r_ref about 6.5 a0
# at count=2000/seed=SEED -- checked empirically, not just assumed; the
# n* contraction of the Slater-fallback heavy atoms keeps Cs/Au/U below
# Li, see pc/validate_atoms.py): its lone 2s1 valence electron is barely
# shielded (low Z_eff) and sits in an n=2 shell, both of which push its
# radius up; this matches real chemistry too (alkali metals are the most
# diffuse/largest atoms in their period). Calibrating off the biggest
# case, at a target smaller than cloud_common.P90_TARGET_PX, keeps every
# other (smaller) element comfortably inside the 240x240 canvas at rest,
# with headroom left for the zoom-breathing swing on top.
_CALIBRATION_Z = 3
_CALIBRATION_TARGET_PX = 70.0


def _p90_radius(xs, ys, zs, percentile=0.90):
    """Same measurement cloud_common.scale_from_radii() makes internally,
    factored out here so scale_for_atom() can use it without also taking
    that function's per-cloud target_px renormalization.
    """
    count = len(xs)
    radii = sorted(math.sqrt(xs[i] * xs[i] + ys[i] * ys[i] + zs[i] * zs[i]) for i in range(count))
    idx = min(count - 1, int(percentile * (count - 1)))
    return radii[idx] if radii[idx] > 1e-6 else 1.0


def scale_for_atom(xs, ys, zs, pixels_per_bohr, amplitude_fraction=cloud_common.ZOOM_AMPLITUDE_FRACTION):
    """Like cloud_common.scale_from_radii(), but with a FIXED base_scale
    (pixels_per_bohr, the SAME for every element) instead of one
    renormalized per-cloud to a constant target_px. cloud_common's version
    deliberately erases size differences between hydrogen presets, by
    design, so unrelated orbitals all read at a comparable size -- for
    atoms, switching Z is partly meant to SHOW the periodic size trend
    (noble gases small and tight, alkali metals big and diffuse, etc.), so
    it must not be erased the same way. r_ref (used for the bounding-sphere
    overlay too) is still measured per-cloud, same p90 method as
    cloud_common.scale_from_radii().

    Returns (base_scale, zoom_amplitude, r_ref) -- same shape as
    cloud_common.scale_from_radii(), with base_scale always equal to
    pixels_per_bohr regardless of this cloud's own radii.
    """
    r_ref = _p90_radius(xs, ys, zs)
    return pixels_per_bohr, pixels_per_bohr * amplitude_fraction, r_ref


def _calibrate_pixels_per_bohr(target_px=_CALIBRATION_TARGET_PX, reference_z=_CALIBRATION_Z):
    xs, ys, zs, _colors, _shells, _signs, _config = build_atom_point_cloud(reference_z, count=2000, seed=SEED)
    return target_px / _p90_radius(xs, ys, zs)

# One color/letter per shell (principal quantum number n) -- historical
# K/L/M/N/O/P/Q shell letters, so shells read as visually distinct "layers"
# the way cloud_common.py's phase coloring reads as lobes. Index 0 unused
# (n starts at 1); the last entry is a fallback for n>7, unreachable for any
# z<= slater.MAX_Z ground-state configuration but kept so an out-of-range n
# degrades to a color/letter instead of an IndexError. Kept as two parallel
# tuples (not one tuple of pairs) since SHELL_RGB alone predates
# SHELL_LETTERS (added for pc/atom_view_pc.py's shell-dissection labels) and
# every existing SHELL_RGB[n] call site would otherwise need a [0]/[1] index
# added.
SHELL_RGB = (
    (255, 255, 255),  # unused (n=0)
    (255, 80, 80),     # n=1 K
    (255, 170, 60),    # n=2 L
    (255, 230, 60),    # n=3 M
    (120, 230, 90),    # n=4 N
    (70, 200, 220),    # n=5 O
    (110, 130, 255),   # n=6 P
    (200, 100, 240),   # n=7 Q
    (160, 160, 160),   # fallback, n>7
)
SHELL_LETTERS = ('?', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', '?')


def title_for_atom(z, config=None):
    if config is None:
        config = slater.electron_configuration(z)
    return "%s (Z=%d) %s" % (slater.element_symbol(z), z, slater.configuration_str(config))


def _split_counts(weights, total):
    """Divide `total` points across groups proportional to `weights` (each
    group's electron count), largest-remainder method so counts sum to
    EXACTLY total instead of drifting from rounding each share
    independently.
    """
    grand_total = sum(weights)
    shares = [total * w / grand_total for w in weights]
    counts = [int(s) for s in shares]
    remainder = total - sum(counts)
    order = sorted(range(len(shares)), key=lambda i: shares[i] - counts[i], reverse=True)
    for i in order[:remainder]:
        counts[i] += 1
    return counts


def _drawing_groups(config):
    """Expand `config` (list of (n, ell, occ) subshells) into per-drawing
    groups (n, ell, m, weight): weight is the electron count that group
    represents, used both to size its share of the total point budget and
    (equally) split across its points.

    A FULL subshell (occ == its capacity 2*(2*ell+1)) becomes ONE group
    with m=None -- signals the isotropic path (see build_atom_point_cloud()
    below), exact for a full subshell and cheaper than building 2*ell+1
    separate angular tables for a subshell whose total density has no
    anisotropy anyway.

    A subshell that is NOT full expands into one group per individually
    occupied real orbital, per slater.hund_fill_m() -- see this module's
    docstring for why.
    """
    groups = []
    for n, ell, occ in config:
        capacity = 2 * (2 * ell + 1)
        if occ == capacity:
            groups.append((n, ell, None, occ))
        else:
            for m, occ_m in slater.hund_fill_m(ell, occ):
                groups.append((n, ell, m, occ_m))
    return groups


def build_atom_point_cloud(z, count=N_POINTS, seed=SEED):
    """Sample `count` points approximating atomic number z's total electron
    density (see module docstring for the model).

    Returns (xs, ys, zs, colors, shells, signs, config): config is
    slater.electron_configuration(z), handed back for title/debug use;
    colors is a plain list of (r,g,b) tuples, one per point, by shell (see
    SHELL_RGB); shells is a plain list of the same length giving each
    point's principal quantum number n -- lets a caller (e.g.
    pc/atom_view_pc.py's shell-dissection view) pick out one shell's points
    without reverse-engineering it from colors (lossy above n=7, see
    SHELL_RGB's fallback entry). signs is an array('b') of the same length:
    +1/-1 for a point sampled from an anisotropic (Hund's-rule per-orbital)
    group, the sign of psi_real() AT THAT POINT (see module docstring's
    Coloring paragraph); 0 for a point from an isotropic (full-subshell)
    group, which has no meaningful sign.
    """
    config = slater.electron_configuration(z)
    groups = _drawing_groups(config)
    display_weights = [
        weight * PARTIAL_SUBSHELL_BOOST if m is not None else weight
        for _, _, m, weight in groups
    ]
    counts = _split_counts(display_weights, count)

    xs = array.array('f', bytes(4 * count))
    ys = array.array('f', bytes(4 * count))
    zs = array.array('f', bytes(4 * count))
    colors = [None] * count
    shells = [0] * count
    signs = array.array('b', bytes(count))

    rng = pointcloud.XorShift32(seed)
    idx = 0
    for (n, ell, m, _weight), group_count in zip(groups, counts):
        # Effective charge for the radial substitution: Clementi-Raimondi Z_eff
        # where the table covers the subshell (Z <= 54), else Slater's Z_eff
        # rescaled by n/n* (Slater's n* consistency) -- see slater.z_eff_radial().
        # The SAME value feeds both the radial sampler and the sign recomputation
        # below, so the psi_real() substitution always matches the table the point
        # was drawn from.
        z_eff = slater.z_eff_radial(z, config, n, ell)
        rgb = SHELL_RGB[n] if n < len(SHELL_RGB) else SHELL_RGB[-1]

        if m is None:
            inv_r_table, _max_r = pointcloud.init_radial_sampler(n, ell, z_eff)
            for _ in range(group_count):
                x, y, pz = pointcloud.sample_isotropic_point(inv_r_table, rng)
                xs[idx] = x
                ys[idx] = y
                zs[idx] = pz
                colors[idx] = rgb
                shells[idx] = n
                idx += 1
        else:
            radial_coeff = orbitals.laguerre_coeffs(n, ell)
            legendre_coeff = orbitals.legendre_coeffs(ell, m)
            sampler = pointcloud.init_orbital_sampler(n, ell, m, z_eff)
            for _ in range(group_count):
                x, y, pz = pointcloud.sample_orbital_point(sampler, rng)
                xs[idx] = x
                ys[idx] = y
                zs[idx] = pz
                colors[idx] = rgb
                shells[idx] = n

                # Sign of the real wavefunction at this point -- NOT
                # available from the sample itself (sample_orbital_point()
                # draws from |psi|^2, which loses it), so recomputed here.
                # z_eff*r matches the substitution init_orbital_sampler()
                # used to build the radial table this point came from;
                # theta/phi are never Z-scaled (see that function's
                # docstring), so the raw sampled angle is correct as-is.
                r = math.sqrt(x * x + y * y + pz * pz)
                theta = math.acos(pz / r) if r > 1e-9 else 0.0
                phi = math.atan2(y, x)
                psi = orbitals.psi_real(z_eff * r, theta, phi, n, ell, m, radial_coeff, legendre_coeff)
                signs[idx] = 1 if psi >= 0.0 else -1

                idx += 1

    return xs, ys, zs, colors, shells, signs, config


def shell_dissection_plan(xs, ys, zs, shells, config):
    """Outer-to-inner breakdown of `config` for pc/atom_view_pc.py's
    shell-dissection view: one entry per distinct principal quantum number
    n present, sorted DESCENDING (outermost/highest n first, since the
    dissection zooms from the outside in).

    Returns a list of (n, letter, subshell_str, electron_count, r_ref):
      - letter: SHELL_LETTERS[n] (K/L/M/...).
      - subshell_str: e.g. "2s2 2p2" -- just this shell's slice of
        slater.configuration_str(config).
      - electron_count: total electrons in shell n (sum of every subshell's
        occupancy at that n).
      - r_ref: p90 radius (see _p90_radius()) of THIS shell's own points
        only, i.e. how far this shell's sampled points actually reach in
        THIS specific point cloud (not a hydrogenic formula) -- what the
        dissection view zooms each shell's disc to fill the frame with.
    """
    by_n = {}
    for n, ell, occ in config:
        by_n.setdefault(n, []).append((ell, occ))

    plan = []
    for n in sorted(by_n.keys(), reverse=True):
        subshells = sorted(by_n[n])
        electron_count = sum(occ for _ell, occ in subshells)
        subshell_str = " ".join(
            "%s%d" % (slater.subshell_label(n, ell), occ) for ell, occ in subshells)
        letter = SHELL_LETTERS[n] if n < len(SHELL_LETTERS) else SHELL_LETTERS[-1]

        shell_xs = [xs[i] for i in range(len(shells)) if shells[i] == n]
        shell_ys = [ys[i] for i in range(len(shells)) if shells[i] == n]
        shell_zs = [zs[i] for i in range(len(shells)) if shells[i] == n]
        r_ref = _p90_radius(shell_xs, shell_ys, shell_zs) if shell_xs else 1.0

        plan.append((n, letter, subshell_str, electron_count, r_ref))
    return plan


# Computed once at import time (see _calibrate_pixels_per_bohr()'s call
# site requirement: build_atom_point_cloud() must already be defined).
PIXELS_PER_BOHR = _calibrate_pixels_per_bohr()
