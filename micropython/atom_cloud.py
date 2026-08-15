"""Multi-electron atom point-cloud approximation, built on top of
orbitals.py/pointcloud.py's hydrogenic math and slater.py's effective-
nuclear-charge model. PC-only for now (see pc/atom_view_pc.py) -- not yet
wired into the ESP32 firmware path.

Model, in one paragraph: for atomic number Z, fill subshells by the Madelung
rule (slater.electron_configuration()), give every electron in a subshell
the SAME effective nuclear charge Z_eff (slater.slater_z_eff(), one value
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
point count split proportional to how many electrons that group represents.

Coloring: every point is colored by shell (principal quantum number n, see
SHELL_RGB) regardless of which path produced it, so the classic K/L/M/N
shell structure reads visually across the whole atom. No phase/sign coloring
(unlike cloud_common.level_to_rgb()) -- would be a reasonable follow-up for
the per-orbital (Hund's-rule) groups, which do have a real signed
wavefunction, but isotropic groups have none (angle-averaging erases it),
and mixing signed/unsigned coloring across one atom's cloud would be more
confusing than informative without also making full-subshell points
somehow report "no sign" -- left as future work.

No point-turnover/resample here either (unlike cloud_common.ResampleState)
-- the cloud is built once and stays static; see pc/atom_view_pc.py's
module docstring for that tradeoff.
"""

import array
import math

import cloud_common
import pointcloud
import slater

N_POINTS = 10000  # PC default; matches pc/orbital_view_pc.py's hydrogen-preset count
SEED = 12345

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
# produces across the whole Z=1..118 range it supports (r_ref about 5.5 --
# checked empirically, not just assumed): its lone 2s1 valence electron is
# barely shielded (low Z_eff) and sits in an n=2 shell, both of which push
# its radius up; this matches real chemistry too (alkali metals are the
# most diffuse/largest atoms in their period). Calibrating off the biggest
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
    xs, ys, zs, _colors, _config = build_atom_point_cloud(reference_z, count=2000, seed=SEED)
    return target_px / _p90_radius(xs, ys, zs)

# One color per shell (principal quantum number n) -- historical K/L/M/N/O/P/Q
# shell letters, so shells read as visually distinct "layers" the way
# cloud_common.py's phase coloring reads as lobes. Index 0 unused (n starts
# at 1); the last entry is a fallback for n>7, unreachable for any z<=
# slater.MAX_Z ground-state configuration but kept so an out-of-range n
# degrades to a color instead of an IndexError.
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

    Returns (xs, ys, zs, colors, config): config is
    slater.electron_configuration(z), handed back for title/debug use;
    colors is a plain list of (r,g,b) tuples, one per point, by shell (see
    SHELL_RGB).
    """
    config = slater.electron_configuration(z)
    groups = _drawing_groups(config)
    counts = _split_counts([weight for _, _, _, weight in groups], count)

    xs = array.array('f', bytes(4 * count))
    ys = array.array('f', bytes(4 * count))
    zs = array.array('f', bytes(4 * count))
    colors = [None] * count

    rng = pointcloud.XorShift32(seed)
    idx = 0
    for (n, ell, m, _weight), group_count in zip(groups, counts):
        z_eff = slater.slater_z_eff(z, config, n, ell)
        rgb = SHELL_RGB[n] if n < len(SHELL_RGB) else SHELL_RGB[-1]

        if m is None:
            inv_r_table, _max_r = pointcloud.init_radial_sampler(n, ell, z_eff)
            for _ in range(group_count):
                x, y, pz = pointcloud.sample_isotropic_point(inv_r_table, rng)
                xs[idx] = x
                ys[idx] = y
                zs[idx] = pz
                colors[idx] = rgb
                idx += 1
        else:
            sampler = pointcloud.init_orbital_sampler(n, ell, m, z_eff)
            for _ in range(group_count):
                x, y, pz = pointcloud.sample_orbital_point(sampler, rng)
                xs[idx] = x
                ys[idx] = y
                zs[idx] = pz
                colors[idx] = rgb
                idx += 1

    return xs, ys, zs, colors, config


# Computed once at import time (see _calibrate_pixels_per_bohr()'s call
# site requirement: build_atom_point_cloud() must already be defined).
PIXELS_PER_BOHR = _calibrate_pixels_per_bohr()
