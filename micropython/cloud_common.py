"""Hardware-agnostic orbital point-cloud logic, shared between the ESP32
firmware (orbital_view.py) and the PC debug simulator (pc/orbital_view_pc.py).
No framebuf/display/st7789py/machine imports here on purpose -- this module
must import cleanly under both real MicroPython and CPython (see
pc/micropython_shim.py), so it stays float-only and hardware-free. ESP32-
specific concerns (Q8 fixed-point conversion, RGB565 packing, viper
rendering) live in orbital_view.py; PC-specific rendering lives in
pc/orbital_view_pc.py. Both call into this module for the same sampling,
ranking, scale-from-radii, and point-turnover logic, so changing the
algorithm once changes it for both.
"""

import array
import math

import orbitals
import pointcloud

ORBITAL_N = 2
ORBITAL_ELL = 1
ORBITAL_M = 0
N_POINTS = 3000
SEED = 12345  # fixed for a reproducible-looking demo across boots

# (n, ell, m, label) presets cycled through on a nudge/keypress.
#
# ORBITALI.md's "n<=3, hardcode closed-form" recommendation is about the
# *on-device C++* path (not yet built); this MicroPython path instead calls
# orbitals.py's general laguerre_coeffs()/legendre_coeffs() at load time
# (valid for any 0<=ell<=n-1, -ell<=m<=ell, n up to orbitals.N_MAX=16), so
# it isn't limited to closed-form n<=3 -- the catalog below goes to n=4 for
# variety, not as a hard ceiling.
#
# psi_real()'s m>=0 -> cos(m*phi) / m<0 -> sin(|m|*phi) convention already
# *is* the real-spherical-harmonic combination ORBITALI.md SS3 describes by
# linear combination (p_x/p_y, d_xz/d_yz, d_x2-y2/d_xy) -- no separate
# combining step needed, so labels below use the textbook chemistry names.
#
# index 6 (2p_z) is the original default orbital.
ORBITAL_PRESETS = (
    (1, 0, 0, "1s"),
    (2, 0, 0, "2s"),
    (2, 1, 1, "2p_x"),
    (2, 1, -1, "2p_y"),
    (2, 1, 0, "2p_z"),
    (3, 0, 0, "3s"),
    (3, 1, 1, "3p_x"),
    (3, 1, -1, "3p_y"),
    (3, 1, 0, "3p_z"),
    (3, 2, 0, "3d_z2"),
    (3, 2, 1, "3d_xz"),
    (3, 2, -1, "3d_yz"),
    (3, 2, 2, "3d_x2-y2"),
    (3, 2, -2, "3d_xy"),
    (4, 3, 0, "4f_z3"),
    (5, 2, 0, "5d_z2"),
    # Rest of n=4 (n=1..3 above were already a complete set); index-matched to
    # src/orbital_library.h's kOrbitalLibrary -- keep both lists appended in the same order.
    (4, 0, 0, "4s"),
    (4, 1, 1, "4p_x"),
    (4, 1, -1, "4p_y"),
    (4, 1, 0, "4p_z"),
    (4, 2, 0, "4d_z2"),
    (4, 2, 1, "4d_xz"),
    (4, 2, -1, "4d_yz"),
    (4, 2, 2, "4d_x2-y2"),
    (4, 2, -2, "4d_xy"),
    (4, 3, 1, "4f_xz2"),
    (4, 3, -1, "4f_yz2"),
    (4, 3, 2, "4f_zx2-y2"),
    (4, 3, -2, "4f_xyz"),
    (4, 3, 3, "4f_xx2-3y2"),
    (4, 3, -3, "4f_y3x2-y2"),
    # Selected n=5/n=6 orbitals (not the full set -- n=5/6 add far more (n,l,m)
    # combinations than n<=4; these are the specific ones requested).
    (5, 1, 0, "5p_z"),
    (5, 2, 1, "5d_xz"),
    (5, 2, 2, "5d_x2-y2"),
    (6, 0, 0, "6s"),
    (6, 1, 0, "6p_z"),
)
DEFAULT_PRESET_INDEX = 4

COLOR_MIN_LEVEL = 80  # "should be bright colors" (2026-08-17) -- raised from 60 so even the dimmest points read clearly
COLOR_MAX_LEVEL = 255

# Projection scale target: the p90-radius point should land P90_TARGET_PX
# out from center. Measured per preset (scale_from_radii()) rather than a
# global constant, since radial extent depends on n, l, AND m, not n alone.
P90_TARGET_PX = 100.0
ZOOM_AMPLITUDE_FRACTION = 0.4

CULL_FRACTION = 0.01        # point-turnover: fraction of the cloud resampled...
CULL_REFRESH_FRAMES = 3     # ...every this many frames
BUZZ_FRACTION = 0.40        # per-frame flicker fraction (device-only effect; see orbital_view.py)

# 1 Bohr radius in picometers (CODATA a0 = 0.52917721090(80)e-10 m) -- lets
# orbital_view.py (device), pc/orbital_view_pc.py (PC), and atom_cloud.py
# (which re-exports this same value under its own name, both modules
# already importing cloud_common regardless) report a scale bar's physical
# size in a unit chemists actually use. Every length these two orbital
# modules work with is implicitly in Bohr radii already (ORBITAL_PRESETS
# are plain hydrogen, Z=1 -- no Z_eff substitution needed, unlike
# atom_cloud.py's multi-electron atoms). pm, not Angstrom: PIL's default
# bitmap font (PC) and framebuf's built-in font (device) both lack a glyph
# for "Å".
PM_PER_BOHR = 52.9177210903

# Bottom-left physical-size reference bar -- shared "nice round length"
# ladder (1/2/5 x a power of ten, the same one a ruler or map scale bar
# uses) plus its PRECOMPUTED display string, so drawing one never needs
# runtime float-to-string formatting ('%g'-style formatting isn't reliably
# available across every MicroPython build/port, unlike CPython -- avoiding
# it here means the exact same table works on both renderers without a
# device-only formatting fallback). Actual drawing (PIL vs framebuf) is
# renderer-specific and stays in orbital_view.py / pc/orbital_view_pc.py;
# only the "which length to show" decision is shared.
SCALE_BAR_CANDIDATES = (
    (0.001, "0.001"), (0.002, "0.002"), (0.005, "0.005"),
    (0.01, "0.01"), (0.02, "0.02"), (0.05, "0.05"),
    (0.1, "0.1"), (0.2, "0.2"), (0.5, "0.5"),
    (1, "1"), (2, "2"), (5, "5"),
    (10, "10"), (20, "20"), (50, "50"),
    (100, "100"), (200, "200"), (500, "500"), (1000, "1000"),
)


def pick_scale_bar_length(pixels_per_unit, max_bar_px):
    """Largest (value, label) candidate from SCALE_BAR_CANDIDATES whose
    on-screen length (value * pixels_per_unit) still fits under max_bar_px
    -- i.e. the most precise round number the bar can show without
    overflowing. Falls back to the smallest candidate if even that one
    would be too long (only possible at extreme zoom-in, where the bar is
    allowed to overflow max_bar_px rather than disappear to zero length).
    """
    best = SCALE_BAR_CANDIDATES[0]
    for candidate in SCALE_BAR_CANDIDATES:
        if candidate[0] * pixels_per_unit <= max_bar_px:
            best = candidate
        else:
            break
    return best


def title_for_preset(preset):
    n, ell, m, label = preset
    return "%s (n=%d l=%d m=%d)" % (label, n, ell, m)


def build_point_cloud(n, ell, m, count=N_POINTS, seed=SEED):
    """Sample `count` points from the (n, ell, m) orbital's probability
    density via inverse-CDF sampling, plus psi^2 (unnormalized) and
    sign(psi) at each. psi_real() returns the *signed* real wavefunction --
    psi2 alone (used for brightness ranking) loses that sign the instant
    it's squared, so it's captured separately here for level_to_rgb()'s
    phase coloring (see ORBITALI.md SS3 / CLAUDE.md SS8: which lobe is
    "positive" vs "negative" is only meaningful before squaring).
    Also returns sampler/rng/radial_coeff/legendre_coeff so
    resample_levels() can keep drawing from the same distribution later.
    """
    sampler = pointcloud.init_orbital_sampler(n, ell, m)
    rng = pointcloud.XorShift32(seed)
    radial_coeff = orbitals.laguerre_coeffs(n, ell)
    legendre_coeff = orbitals.legendre_coeffs(ell, m)

    xs = array.array('f', bytes(4 * count))
    ys = array.array('f', bytes(4 * count))
    zs = array.array('f', bytes(4 * count))
    psi2 = array.array('f', bytes(4 * count))
    signs = array.array('b', bytes(count))

    for i in range(count):
        x, y, z = pointcloud.sample_orbital_point(sampler, rng)
        xs[i] = x
        ys[i] = y
        zs[i] = z
        r = math.sqrt(x * x + y * y + z * z)
        theta = math.acos(z / r) if r > 1e-9 else 0.0
        phi = math.atan2(y, x)
        psi = orbitals.psi_real(r, theta, phi, n, ell, m, radial_coeff, legendre_coeff)
        psi2[i] = psi * psi
        signs[i] = 1 if psi >= 0.0 else -1

    return xs, ys, zs, psi2, signs, sampler, rng, radial_coeff, legendre_coeff


# Phase colors -- sign of psi_real determines the color, and EVERY orbital
# uses the same fixed positive/negative pair, consistently across presets,
# so a viewer learns the sign-color association once and can read it at a
# glance across the whole library (superseded an older scheme where each
# preset had its own distinguishing hue). Index-matched to ORBITAL_PRESETS
# and to src/orbital_library.h's OrbitalDescriptor.posRgb565/negRgb565 --
# keep the two in lockstep: these are the same 8-bit RGB values as
# Display::kColorOrbitalRed (210, 40, 40) / kColorOrbitalBlue (40, 80, 210)
# in src/display.h (the device's own named palette, not plain 0/255 primaries
# -- see that file's history for why). s orbitals (ell=0) are single-signed
# everywhere, so they render as a uniform cloud in the positive (red) color
# with no visible split -- expected, not a bug (no phase change without a
# node).
ORBITAL_PHASE_COLORS = (
    ((210, 40, 40), (40, 80, 210)),    # 0 1s
    ((210, 40, 40), (40, 80, 210)),    # 1 2s
    ((210, 40, 40), (40, 80, 210)),    # 2 2p_x
    ((210, 40, 40), (40, 80, 210)),    # 3 2p_y
    ((210, 40, 40), (40, 80, 210)),    # 4 2p_z (default preset)
    ((210, 40, 40), (40, 80, 210)),    # 5 3s
    ((210, 40, 40), (40, 80, 210)),    # 6 3p_x
    ((210, 40, 40), (40, 80, 210)),    # 7 3p_y
    ((210, 40, 40), (40, 80, 210)),    # 8 3p_z
    ((210, 40, 40), (40, 80, 210)),    # 9 3d_z2
    ((210, 40, 40), (40, 80, 210)),    # 10 3d_xz
    ((210, 40, 40), (40, 80, 210)),    # 11 3d_yz
    ((210, 40, 40), (40, 80, 210)),    # 12 3d_x2-y2
    ((210, 40, 40), (40, 80, 210)),    # 13 3d_xy
    ((210, 40, 40), (40, 80, 210)),    # 14 4f_z3
    ((210, 40, 40), (40, 80, 210)),    # 15 5d_z2
    ((210, 40, 40), (40, 80, 210)),    # 16 4s
    ((210, 40, 40), (40, 80, 210)),    # 17 4p_x
    ((210, 40, 40), (40, 80, 210)),    # 18 4p_y
    ((210, 40, 40), (40, 80, 210)),    # 19 4p_z
    ((210, 40, 40), (40, 80, 210)),    # 20 4d_z2
    ((210, 40, 40), (40, 80, 210)),    # 21 4d_xz
    ((210, 40, 40), (40, 80, 210)),    # 22 4d_yz
    ((210, 40, 40), (40, 80, 210)),    # 23 4d_x2-y2
    ((210, 40, 40), (40, 80, 210)),    # 24 4d_xy
    ((210, 40, 40), (40, 80, 210)),    # 25 4f_xz2
    ((210, 40, 40), (40, 80, 210)),    # 26 4f_yz2
    ((210, 40, 40), (40, 80, 210)),    # 27 4f_zx2-y2
    ((210, 40, 40), (40, 80, 210)),    # 28 4f_xyz
    ((210, 40, 40), (40, 80, 210)),    # 29 4f_xx2-3y2
    ((210, 40, 40), (40, 80, 210)),    # 30 4f_y3x2-y2
    ((210, 40, 40), (40, 80, 210)),    # 31 5p_z
    ((210, 40, 40), (40, 80, 210)),    # 32 5d_xz
    ((210, 40, 40), (40, 80, 210)),    # 33 5d_x2-y2
    ((210, 40, 40), (40, 80, 210)),    # 34 6s
    ((210, 40, 40), (40, 80, 210)),    # 35 6p_z
)
assert len(ORBITAL_PHASE_COLORS) == len(ORBITAL_PRESETS)

# Universal fallback pair kept for any caller that doesn't pick a preset pair
# (e.g. atom_view's phase-colored highlights) -- same red/blue pair, now
# identical to every entry in ORBITAL_PHASE_COLORS above.
PHASE_POSITIVE_RGB = ORBITAL_PHASE_COLORS[4][0]
PHASE_NEGATIVE_RGB = ORBITAL_PHASE_COLORS[4][1]


def level_to_rgb(level, sign, phase_pair=None):
    """Brightness level + wavefunction sign -> (r, g, b): scales the
    phase-color pair (per-preset bright pair by default; the universal
    PHASE_POSITIVE_RGB/PHASE_NEGATIVE_RGB when phase_pair is None) by
    level/255 so COLOR_MIN_LEVEL's "keep dim points visible" floor still
    applies per-channel. Each platform encodes the result into its own pixel
    format (RGB565+swap on the device, a plain tuple on PC).
    """
    if phase_pair is None:
        base = PHASE_POSITIVE_RGB if sign >= 0 else PHASE_NEGATIVE_RGB
    else:
        base = phase_pair[0] if sign >= 0 else phase_pair[1]
    return (base[0] * level // 255, base[1] * level // 255, base[2] * level // 255)


def compute_levels(psi2, min_level=COLOR_MIN_LEVEL, max_level=COLOR_MAX_LEVEL):
    """Rank-based (histogram-equalized) brightness levels for each point.
    NOT linear min/max: psi^2 is heavily right-skewed (a handful of samples
    near the nucleus dominate the range), so a linear scale compresses most
    points into the dim end. Sorting and assigning brightness by rank/count
    spreads every point evenly across [min_level, max_level] instead.

    Returns (levels, psi2_sorted): levels is array('B'), one brightness
    value per input point (same order as psi2); psi2_sorted is the sorted
    psi^2 values alone, kept as a frozen reference so resample_levels() can
    bisect a new point's rank without re-sorting the whole cloud.
    """
    count = len(psi2)
    ranked = sorted((psi2[i], i) for i in range(count))
    span = max_level - min_level
    denom = count - 1 if count > 1 else 1

    levels = array.array('B', bytes(count))
    psi2_sorted = array.array('f', bytes(4 * count))
    for rank in range(count):
        value, i = ranked[rank]
        psi2_sorted[rank] = value
        levels[i] = min_level + (rank * span) // denom
    return levels, psi2_sorted


def scale_from_radii(xs, ys, zs, target_px=P90_TARGET_PX, percentile=0.90,
                      amplitude_fraction=ZOOM_AMPLITUDE_FRACTION):
    """Projection scale measured from this preset's own sampled radii: finds
    the `percentile`-th radius and picks a scale so it lands at `target_px`.
    amplitude is kept proportional to base_scale so zoom breathing never
    shrinks a preset to a "too far away" dot regardless of its physical size.

    Returns (base_scale, zoom_amplitude, r_ref) -- r_ref is the reference
    radius itself (unscaled), used by pc/orbital_view_pc.py for its
    bounding-sphere overlay.
    """
    count = len(xs)
    radii = sorted(math.sqrt(xs[i] * xs[i] + ys[i] * ys[i] + zs[i] * zs[i]) for i in range(count))
    idx = min(count - 1, int(percentile * (count - 1)))
    r_ref = radii[idx] if radii[idx] > 1e-6 else 1.0
    base_scale = target_px / r_ref
    return base_scale, base_scale * amplitude_fraction, r_ref


def bisect_rank(sorted_values, value):
    """Index where `value` would insert into ascending `sorted_values` --
    manual binary search, not the `bisect` module (not uniformly available
    across MicroPython builds).
    """
    lo = 0
    hi = len(sorted_values)
    while lo < hi:
        mid = (lo + hi) // 2
        if sorted_values[mid] < value:
            lo = mid + 1
        else:
            hi = mid
    return lo


class ResampleState:
    """What resample_levels() needs to keep drawing from the same
    distribution as the initial cloud. `cursor` round-robins through point
    indices so every point turns over in turn, rather than a random choice
    leaving some indices stale indefinitely.
    """

    def __init__(self, sampler, rng, radial_coeff, legendre_coeff, n, ell, m, psi2_sorted):
        self.sampler = sampler
        self.rng = rng
        self.radial_coeff = radial_coeff
        self.legendre_coeff = legendre_coeff
        self.n = n
        self.ell = ell
        self.m = m
        self.psi2_sorted = psi2_sorted
        self.cursor = 0


def resample_levels(state, xs, ys, zs, count):
    """Replace `count` points (round-robin via state.cursor) with fresh
    samples from state's orbital distribution, updating xs/ys/zs in place.
    Returns a list of (idx, level, sign) triples for the caller to turn into
    its own pixel-color format (via level_to_rgb(level, sign)) at each
    changed index.

    level can exceed COLOR_MAX_LEVEL when a fresh sample's psi^2 beats
    everything in the frozen reference (psi^2 is heavily right-skewed --
    see compute_levels() -- so eventually happening by chance is expected
    over a long-running resample, not a bug); callers must clamp before
    encoding to a format that doesn't itself clamp (e.g. RGB565 packing
    wraps instead).
    """
    n = len(xs)
    sampler = state.sampler
    rng = state.rng
    radial_coeff = state.radial_coeff
    legendre_coeff = state.legendre_coeff
    quantum_n, quantum_ell, quantum_m = state.n, state.ell, state.m
    psi2_sorted = state.psi2_sorted
    sorted_span = len(psi2_sorted) - 1 if len(psi2_sorted) > 1 else 1
    level_span = COLOR_MAX_LEVEL - COLOR_MIN_LEVEL

    changed = []
    for _ in range(count):
        idx = state.cursor
        state.cursor = idx + 1 if idx + 1 < n else 0

        x, y, z = pointcloud.sample_orbital_point(sampler, rng)
        xs[idx] = x
        ys[idx] = y
        zs[idx] = z

        r = math.sqrt(x * x + y * y + z * z)
        theta = math.acos(z / r) if r > 1e-9 else 0.0
        phi = math.atan2(y, x)
        psi = orbitals.psi_real(r, theta, phi, quantum_n, quantum_ell, quantum_m,
                                 radial_coeff, legendre_coeff)
        psi2 = psi * psi

        rank = bisect_rank(psi2_sorted, psi2)
        level = COLOR_MIN_LEVEL + (rank * level_span) // sorted_span
        sign = 1 if psi >= 0.0 else -1
        changed.append((idx, level, sign))
    return changed
