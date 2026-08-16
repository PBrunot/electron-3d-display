"""Multi-electron atom point-cloud approximation, built on top of
orbitals.py/pointcloud.py's hydrogenic math and slater.py's effective-
nuclear-charge model. Platform-agnostic (no PC- or device-only imports),
consumed by both pc/atom_view_pc.py and micropython/atom_view.py -- the
latter is not the default boot animation (see main.py's docstring), but the
model itself needs no adaptation to run on-device.

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
point count split STRICTLY proportional to how many electrons that group
represents (see _split_counts()) -- no per-group boosting of any kind. An
earlier version of this module multiplied anisotropic (partially-filled)
groups' share by a constant (PARTIAL_SUBSHELL_BOOST) to make their lobed
shape read more clearly against neighboring isotropic groups' density in
the merged view; removed because it broke the (n, ell)-level
point-count/electron-count proportionality pc/atom_view_pc.py's per-subshell
dissection view relies on to be honest -- with the boost, two subshells
holding the SAME electron count (e.g. iron's 3d6 vs 3p6) could render with a
3x DIFFERENT point count purely because one happened to be anisotropic,
which reads as "shell with more electrons has more points" being violated
even though it wasn't, per electron count. A partially-filled subshell can
still look visually weaker than a full one of similar size in the merged
(non-dissection) view as a result -- true again now, same as the version
before the boost existed -- but every point count in this module is now
exactly what the electron count says it should be, in every view.

Coloring: every point's default color (SHELL_RGB) is still by shell
(principal quantum number n) regardless of which path produced it, so the
classic K/L/M/N shell structure reads visually across the whole atom. The
points belonging to the OUTERMOST subshell (outer_subshell_r_ref()'s own
plan[0] -- the same subshell the bounding circle/scale calibration already
treats as "the atom's size", see that function's docstring) are additionally
brightened (_brighten_outer_shell(), toward white) after the fact, while
every OTHER point is dimmed the other way (_dim_inner_shell(), toward black)
to widen the contrast further. Without this, the outer/valence subshell is
easy to miss entirely: for any atom past Ne it is a small minority of the
total point BUDGET too (points are split strictly proportional to electron
count, see _split_counts() -- e.g. potassium's single valence 4s electron is
1 of 19, ~5% of its points), so the correctly-larger valence cloud reads as
visually similar-sized "blob" to every other element's core-dominated cloud
unless its own points are made to stand out individually. Dimming the inner
layers by default is safe, not lossy: each one still gets its own
full-brightness moment when the shell-dissection view (pc/atom_view_pc.py's
D-key sequence) zooms into it specifically -- see below. Every
point ALSO carries a signed-wavefunction sign (see `signs`, below) wherever
one is meaningful: isotropic (full-subshell) groups have none -- angle-
averaging erases it, same reasoning Unsoeld's theorem relies on above -- and
are marked sign=0; anisotropic (Hund's-rule per-orbital) groups DO have a
real signed wavefunction, recovered here by evaluating orbitals.psi_real()
at each sampled point (same z_eff*r substitution the radial table itself
was built with, see init_orbital_sampler()'s docstring) after the fact --
the inverse-CDF sampling that placed the point only ever used |psi|^2,
which loses the sign, so it has to be recomputed, not reused. Not applied
to `colors` itself (still the plain outer-brightened/inner-dimmed SHELL_RGB
above, unconditionally, never phase) since mixing signed/unsigned coloring
across one atom's cloud by default would be more confusing than
informative; instead left for a caller that wants it for a specific purpose
to use `signs` selectively -- see pc/atom_view_pc.py's shell-dissection
view, which swaps to phase coloring
(cloud_common.PHASE_POSITIVE_RGB/PHASE_NEGATIVE_RGB) only for the ONE shell
currently exploded, where "isotropic groups have none" doesn't collide with
anything else on screen -- and only for its anisotropic points; sign=0
points there fall back to that subshell's true (undimmed/unbrightened)
SHELL_RGB, not `colors[i]` -- see render_dissection_frame()'s comment.

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

N_POINTS = 15000  # PC default; matches pc/orbital_view_pc.py's hydrogen-preset count
SEED = 12345

# Bohr radius, re-exported from cloud_common (single source of truth --
# atom_cloud.py already imports cloud_common regardless, see below) so
# pc/atom_view_pc.py's scale bar can read it off either module. Every
# length elsewhere in this module (and in orbitals.py/pointcloud.py) is
# implicitly in Bohr radii (see orbitals.py's module docstring: r is
# physical radius with a0=1, Z=1 folded into the hydrogenic formula).
PM_PER_BOHR = cloud_common.PM_PER_BOHR
ANGSTROM_PER_BOHR = PM_PER_BOHR / 100.0

# Calibration for scale_for_atom() below: reference atomic number and its
# on-screen target size, used by pixels_per_bohr_for_canvas() to derive
# PIXELS_PER_BOHR (a single pixels-per-Bohr-radius conversion factor shared
# by every element -- see scale_for_atom()'s docstring for why this must NOT
# be cloud_common.scale_from_radii()'s per-cloud renormalization).
#
# The target is a FRACTION of the caller's own canvas half-width (CENTER),
# not a fixed pixel count -- this module is shared verbatim between the PC
# debug viewer (pc/atom_view_pc.py, a 480px-wide math buffer) and the real
# ESP32 device (micropython/atom_view.py, a 240px-wide panel); a fixed pixel
# target tuned for one would be wrong on the other (either far too small on
# PC or clipping off the device's smaller screen), whereas the same FRACTION
# of each one's own CENTER reproduces the same relative on-screen size on
# both. Each canvas-owning module calls pixels_per_bohr_for_canvas(CENTER)
# once with its own CENTER and caches the result (see pc/atom_view_pc.py and
# micropython/atom_view.py) -- this module itself has no canvas of its own.
#
# Rubidium (Z=37) is the reference: the largest atom this model produces
# within the Clementi-Raimondi-covered range (Z<=54, see
# slater.z_eff_radial()'s CR/Slater-fallback split) once r_ref is measured
# correctly as outer_subshell_r_ref() (checked empirically across
# Z=1..54 at count=2000/seed=SEED, not just assumed -- alkali metals grow
# monotonically Li(7.2) < Na(7.5) < K(9.1) < Rb(9.3), matching real
# chemistry: atomic radius grows down a group). NOT the true global max
# across the whole Z=1..118 range this model supports -- past Z=54 the
# Slater-fallback path produces a suspicious discontinuity (e.g. Cs's
# computed radius jumps to ~3x Rb's despite being only one step down the
# same group, right at the CR-table cutoff -- a separate, not-yet-
# investigated weakness in slater.z_eff_radial()'s Slater/n* rescaling for
# n>=6, not something outer_subshell_r_ref() itself introduces). Calibrating
# off one of those inflated Slater-fallback atoms instead would have shrunk
# every other (correctly-computed) element on screen to compensate for a
# probably-wrong outlier, so Rb -- the biggest atom in the range this model
# is actually validated against (see pc/validate_atoms.py) -- is the safer
# reference until that separate issue is chased down. 0.6 of CENTER keeps
# every other CR-range element comfortably inside the canvas at rest, with
# headroom left for the zoom-breathing swing on top (0.6 * (1 +
# cloud_common.ZOOM_AMPLITUDE_FRACTION) == 0.84 of CENTER at the outer edge
# of the breathing swing, still short of the canvas edge on either canvas);
# Slater-fallback elements (Z>=55) may still render oversized/clipped until
# the underlying z_eff issue is fixed.
_CALIBRATION_Z = 37
_CALIBRATION_RADIUS_FRACTION = 0.6


def _p90_radius(xs, ys, zs, percentile=0.90):
    """Same measurement cloud_common.scale_from_radii() makes internally,
    factored out here so scale_for_atom() can use it without also taking
    that function's per-cloud target_px renormalization.
    """
    count = len(xs)
    radii = sorted(math.sqrt(xs[i] * xs[i] + ys[i] * ys[i] + zs[i] * zs[i]) for i in range(count))
    idx = min(count - 1, int(percentile * (count - 1)))
    return radii[idx] if radii[idx] > 1e-6 else 1.0


def scale_for_atom(r_ref, pixels_per_bohr, amplitude_fraction=cloud_common.ZOOM_AMPLITUDE_FRACTION):
    """Like cloud_common.scale_from_radii(), but with a FIXED base_scale
    (pixels_per_bohr, the SAME for every element) instead of one
    renormalized per-cloud to a constant target_px. cloud_common's version
    deliberately erases size differences between hydrogen presets, by
    design, so unrelated orbitals all read at a comparable size -- for
    atoms, switching Z is partly meant to SHOW the periodic size trend
    (noble gases small and tight, alkali metals big and diffuse, etc.), so
    it must not be erased the same way.

    Unlike an earlier version of this function, r_ref is now a PARAMETER,
    not measured internally from xs/ys/zs -- callers must pass
    outer_subshell_r_ref()'s result (the OUTERMOST/valence subshell's own
    p90 radius), not the whole cloud's p90. See that function's docstring
    for why: the whole-cloud statistic is dominated by core electrons for
    any atom with more core than valence electrons (i.e. nearly everything
    past helium), which was making the on-screen bounding circle -- and the
    PIXELS_PER_BOHR calibration itself -- badly incoherent for heavier atoms
    (e.g. calcium, whose true valence 4s radius is ~2.2x its old whole-cloud
    r_ref, rendering barely bigger than carbon despite being one of the most
    diffuse atoms in its period in reality).

    Returns (base_scale, zoom_amplitude, r_ref) -- same shape as
    cloud_common.scale_from_radii(), with base_scale always equal to
    pixels_per_bohr and r_ref simply the value passed in, unchanged.
    """
    return pixels_per_bohr, pixels_per_bohr * amplitude_fraction, r_ref


def outer_subshell_r_ref(xs, ys, zs, shells, ells, config):
    """The p90 radius (see _p90_radius()) of just the OUTERMOST subshell --
    plan[0] of subshell_dissection_plan(), i.e. the subshell with the
    largest MEASURED extent in this specific point cloud -- rather than the
    whole cloud's own p90 radius. This is what actually defines an atom's
    physical/chemical size; the whole-cloud statistic does not, for any
    atom with more core electrons than valence ones (every element past
    helium, since _split_counts() gives each subshell a point share
    proportional to its electron count -- see build_atom_point_cloud()).

    Concretely, for calcium (1s2 2s2 2p6 3s2 3p6 4s2 -- only 2 of 20
    electrons in the valence 4s): the whole-cloud p90 lands at ~2.5 a0,
    which is inside the 90%-core population's own bulk and doesn't even
    reach the 4s subshell's own MEDIAN radius (~5.6 a0 -- measured: the
    whole-cloud 90th percentile corresponds to roughly the 4s subshell's own
    95th percentile, i.e. it captures almost none of where the valence
    electron actually spends its time). Same valence-subshell principle
    pc/validate_atoms.py's model-radius comparison already uses (see that
    module's docstring for the citation/reasoning) -- applied here to what
    actually gets drawn on screen (the bounding-circle radius and the
    PIXELS_PER_BOHR calibration below), not just the offline validation
    harness.
    """
    plan = subshell_dissection_plan(xs, ys, zs, shells, ells, config)
    return plan[0][5] if plan else 1.0


def pixels_per_bohr_for_canvas(canvas_center, radius_fraction=_CALIBRATION_RADIUS_FRACTION,
                                reference_z=_CALIBRATION_Z):
    """The single pixels-per-Bohr-radius conversion factor scale_for_atom()
    needs, calibrated so the reference element (Rubidium) reaches
    `radius_fraction` of `canvas_center` at rest -- see the calibration
    comment above for why this takes the canvas's own CENTER instead of a
    fixed pixel count. Call once per canvas (PC and device each have their
    own CENTER) and cache the result -- this repeats build_atom_point_cloud()
    for the reference element, not free.
    """
    xs, ys, zs, _colors, shells, ells, _signs, config = build_atom_point_cloud(
        reference_z, count=2000, seed=SEED)
    target_px = radius_fraction * canvas_center
    return target_px / outer_subshell_r_ref(xs, ys, zs, shells, ells, config)

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

# How far the outermost subshell's points get lerped toward white (0 = no
# change, 1 = pure white) -- see build_atom_point_cloud()'s call to
# _brighten_outer_shell() and the module docstring's Coloring paragraph.
# 0.55 read as only a modest change once actually rendered (PC's
# ELECTRON_ALPHA=0.7 per-hit blending already discounts whatever color a
# sparse, rarely-repeated-pixel point carries, and the device path draws the
# color outright with no discount at all) -- pushed close to 1.0 so a single
# hit is close to pure white, the brightest a pixel can get against the
# black background, while keeping a sliver of the shell hue.
OUTER_SHELL_BRIGHTEN = 0.92


def _brighten_outer_shell(rgb, factor=OUTER_SHELL_BRIGHTEN):
    """Lerp an SHELL_RGB color toward white by `factor` -- makes the
    outermost/valence subshell's points individually stand out from the much
    denser, same-colored-by-shell inner core swarm around them.
    """
    r, g, b = rgb
    return (
        r + int((255 - r) * factor),
        g + int((255 - g) * factor),
        b + int((255 - b) * factor),
    )


# Companion knob to OUTER_SHELL_BRIGHTEN: fraction of brightness removed from
# every OTHER (non-outer) point, widening the contrast further instead of
# relying on the outer boost alone. Deliberately mild ("slightly penalize") --
# unlike the outer shell, the inner/core layers still need to read as their
# own K/L/M/N colors in the normal merged view; they don't lose visibility
# altogether since each one gets its own full-brightness moment in the
# shell-dissection view (pc/atom_view_pc.py's D-key sequence, phase-colored
# and undimmed while active -- see atom_cloud.py's Coloring docstring
# paragraph), so dimming them here by default is safe rather than lossy.
INNER_SHELL_DIM = 0.2


def _dim_inner_shell(rgb, factor=INNER_SHELL_DIM):
    """Scale an SHELL_RGB color toward black by `factor` -- the inverse of
    _brighten_outer_shell(), applied to every point that ISN'T in the
    outermost subshell.
    """
    r, g, b = rgb
    keep = 1.0 - factor
    return (int(r * keep), int(g * keep), int(b * keep))


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

    Returns (xs, ys, zs, colors, shells, ells, signs, config): config is
    slater.electron_configuration(z), handed back for title/debug use;
    colors is a plain list of (r,g,b) tuples, one per point, by shell (see
    SHELL_RGB); shells is a plain list of the same length giving each
    point's principal quantum number n -- lets a caller (e.g.
    pc/atom_view_pc.py's dissection view) pick out one shell's points
    without reverse-engineering it from colors (lossy above n=7, see
    SHELL_RGB's fallback entry). ells is a plain list of the same length
    giving each point's angular momentum quantum number ell, alongside
    shells -- together (shells[i], ells[i]) identifies which SUBSHELL (e.g.
    2p, not just "shell 2") a point came from, for a caller that wants to
    dissect at that finer grain (see subshell_dissection_plan()). signs is
    an array('b') of the same length: +1/-1 for a point sampled from an
    anisotropic (Hund's-rule per-orbital) group, the sign of psi_real() AT
    THAT POINT (see module docstring's Coloring paragraph); 0 for a point
    from an isotropic (full-subshell) group, which has no meaningful sign.
    """
    config = slater.electron_configuration(z)
    groups = _drawing_groups(config)
    counts = _split_counts([weight for _, _, _, weight in groups], count)

    xs = array.array('f', bytes(4 * count))
    ys = array.array('f', bytes(4 * count))
    zs = array.array('f', bytes(4 * count))
    colors = [None] * count
    shells = [0] * count
    ells = [0] * count
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
                ells[idx] = ell
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
                ells[idx] = ell

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

    # Brighten the outermost subshell's points and dim every other one (see
    # _brighten_outer_shell()/_dim_inner_shell() and the module docstring's
    # Coloring paragraph) -- same subshell outer_subshell_r_ref() uses for
    # the bounding circle/scale calibration, so the brightened points are
    # literally the ones that circle bounds.
    plan = subshell_dissection_plan(xs, ys, zs, shells, ells, config)
    if plan:
        n_out, ell_out = plan[0][0], plan[0][1]
        bright_cache = {}
        dim_cache = {}
        for i in range(count):
            base = colors[i]
            if shells[i] == n_out and ells[i] == ell_out:
                bright = bright_cache.get(base)
                if bright is None:
                    bright = _brighten_outer_shell(base)
                    bright_cache[base] = bright
                colors[i] = bright
            else:
                dim = dim_cache.get(base)
                if dim is None:
                    dim = _dim_inner_shell(base)
                    dim_cache[base] = dim
                colors[i] = dim

    return xs, ys, zs, colors, shells, ells, signs, config


def subshell_dissection_plan(xs, ys, zs, shells, ells, config):
    """Outer-to-inner breakdown of `config` for pc/atom_view_pc.py's
    dissection view, one entry per (n, ell) SUBSHELL -- e.g. carbon's L
    shell (2s2 2p2) becomes TWO entries, "2s2" and "2p2", not one combined
    "shell 2" entry -- so each orbital gets its own zoom/label instead of
    sharing a scene with whatever else happens to share its principal
    quantum number.

    Ordered by each subshell's own MEASURED p90 radius in THIS point cloud,
    descending (largest/outermost first) -- NOT by (n, ell) quantum-number
    order. Quantum numbers alone don't reliably predict radial extent for
    multi-electron atoms (e.g. the 4s/3d crossover across the transition
    metals: 4s fills before 3d by the Madelung rule but is not always
    outside it once both are occupied); measuring r_ref directly from this
    cloud's own sampled points sidesteps having to encode that ordering by
    hand and stays correct however slater.z_eff_radial() shifts a given
    element's subshells relative to each other.

    Returns a list of (n, ell, letter, subshell_str, electron_count, r_ref):
      - letter: SHELL_LETTERS[n] (K/L/M/...) -- which shell this subshell
        belongs to, for the on-screen label.
      - subshell_str: e.g. "2p2" (slater.subshell_label(n, ell) + occupancy).
      - electron_count: this subshell's own occupancy (config already
        stores subshells individually -- no summing across ell needed here,
        unlike the old per-shell version this replaces).
      - r_ref: p90 radius (see _p90_radius()) of THIS subshell's own points
        only, i.e. how far its sampled points actually reach in THIS
        specific point cloud (not a hydrogenic formula) -- what the
        dissection view zooms this subshell's disc to fill the frame with.
    """
    plan = []
    for n, ell, occ in config:
        subshell_str = "%s%d" % (slater.subshell_label(n, ell), occ)
        letter = SHELL_LETTERS[n] if n < len(SHELL_LETTERS) else SHELL_LETTERS[-1]

        sub_xs = [xs[i] for i in range(len(shells)) if shells[i] == n and ells[i] == ell]
        sub_ys = [ys[i] for i in range(len(shells)) if shells[i] == n and ells[i] == ell]
        sub_zs = [zs[i] for i in range(len(shells)) if shells[i] == n and ells[i] == ell]
        r_ref = _p90_radius(sub_xs, sub_ys, sub_zs) if sub_xs else 1.0

        plan.append((n, ell, letter, subshell_str, occ, r_ref))

    plan.sort(key=lambda entry: entry[5], reverse=True)
    return plan
