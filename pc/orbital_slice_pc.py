"""Math-validation harness + static PNG preview for the plane-slice heatmap (SLICE.md).

Builds the exact same static-plane cell table src/views/orbital_slice.cpp's
buildSliceTable() builds on-device -- same lobe-plane azimuth ph0, same folded
azimuthal sign flip, same density-normalized brightness mapping (percentile-clipped
|psi|^2 + gamma, see SLICE.md's contrast note) -- using micropython/orbitals.py's
already cross-validated R(r)/P_l^m(theta) primitives, so this script is
checking SLICE.md section 3's *derivation*, not re-deriving the underlying
physics from scratch.

Run before any firmware flash (SLICE.md section 9.1): asserts the section 3
sanity checks (2px/2py dumbbells and their phase split, 2py's slice-at-ph0=0
zero patch, 3dxy's same-phase lobe pair, 3dz2/s-orbital axial symmetry, 2s's
radial node ring), then writes one PNG per checked orbital under
pc/out/slice_preview/ for a by-eye look.

No tkinter window (unlike pc/orbital_view_pc.py) -- this is a one-shot batch
check, not an interactive viewer; PIL alone is enough to produce the PNGs.
"""

import math
import os
import sys

import micropython_shim  # noqa: F401 -- must precede micropython/ imports (see that module)

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'micropython'))

import orbitals

from PIL import Image

GRID_SIZE = 120  # matches src/config/visual_constants.h's kSliceGridSize
OUT_DIR = os.path.join(os.path.dirname(__file__), 'out', 'slice_preview')

# Matches src/config/visual_constants.h's kSliceLevelGamma/kSliceDensityPercentile -- the
# slice's brightness is DENSITY-normalized (percentile-clipped |psi|^2 + gamma), NOT the 3D
# cloud's rank curve (orbitalLevelFromRankFraction): rank-equalizing a uniform grid lights
# half the screen to ~83% brightness by construction (median level 212 for every orbital, 0%
# dark cells -- see SLICE.md's contrast note). The sign/pattern checks below only need a
# monotonic curve, but the PNGs should look like what the device shows.
LEVEL_GAMMA = 0.5
DENSITY_PERCENTILE = 0.999

# Same orange/blue phase pair every device preset uses (Display::kColorOrbitalRed/Blue).
POS_RGB = (210, 40, 40)
NEG_RGB = (40, 80, 210)


def neg_half_sign(m):
    """(-1)^|m| -- the sign the x<0 half of the plane picks up relative to x>0."""
    return 1 if abs(m) % 2 == 0 else -1


def build_slice_table(n, ell, m, extent_bohr, grid_size=GRID_SIZE):
    """Direct port of buildSliceTable() (src/views/orbital_slice.cpp): sample
    base = R(r)*P(theta) at each cell center, fold in the azimuthal sign flip,
    then density-normalize |psi|^2 into a 0..255 brightness against the grid's own
    99.9th percentile with a gamma lift (kSliceLevelGamma/kSliceDensityPercentile).

    Returns (values, signs, levels) as flat grid_size*grid_size lists, row-major,
    row 0 = screen top = +z (matching orbital_slice.cpp's convention).
    """
    radial_coeff = orbitals.laguerre_coeffs(n, ell)
    legendre_coeff = orbitals.legendre_coeffs(ell, m)
    flip = neg_half_sign(m)
    cell = (2.0 * extent_bohr) / grid_size

    signs = [0] * (grid_size * grid_size)
    mags = [0.0] * (grid_size * grid_size)
    for row in range(grid_size):
        z = extent_bohr - (row + 0.5) * cell
        for col in range(grid_size):
            x = -extent_bohr + (col + 0.5) * cell
            r = math.sqrt(x * x + z * z)
            cos_theta = max(-1.0, min(1.0, z / r)) if r > 1e-9 else 0.0
            theta = math.acos(cos_theta)
            base = orbitals.hydrogen_radial_function(r, n, ell, radial_coeff) * orbitals.compute_plm(
                theta, ell, m, legendre_coeff
            )
            value = base * flip if x < 0 else base
            idx = row * grid_size + col
            signs[idx] = 1 if value >= 0 else -1
            mags[idx] = abs(value)

    # Density-normalize |psi|^2 against the grid's own 99.9th percentile, then apply the
    # gamma lift -- same mapping as src/views/orbital_slice.cpp's buildSliceTable().
    dens = [v * v for v in mags]
    srt = sorted(dens)
    v99 = srt[int((len(srt) - 1) * DENSITY_PERCENTILE)]
    if v99 <= 0.0:
        v99 = 1.0
    levels = [min(255, int(255.0 * (min(1.0, d / v99) ** LEVEL_GAMMA))) for d in dens]
    return mags, signs, levels


def save_png(name, signs, levels, grid_size=GRID_SIZE):
    os.makedirs(OUT_DIR, exist_ok=True)
    img = Image.new('RGB', (grid_size, grid_size))
    px = img.load()
    for row in range(grid_size):
        for col in range(grid_size):
            idx = row * grid_size + col
            base = POS_RGB if signs[idx] >= 0 else NEG_RGB
            level = levels[idx] / 255.0
            px[col, row] = tuple(int(c * level) for c in base)
    path = os.path.join(OUT_DIR, f'{name}.png')
    img.resize((grid_size * 2, grid_size * 2), Image.NEAREST).save(path)
    return path


def at(mags, signs, grid_size, row, col):
    idx = row * grid_size + col
    return mags[idx], signs[idx]


def check(label, condition):
    status = 'OK' if condition else 'FAIL'
    print(f'[{status}] {label}')
    assert condition, label


def main():
    g = GRID_SIZE
    left, right = g // 4, g - 1 - g // 4  # off-center columns, well clear of the r=0 cell
    mid = g // 2

    # --- 2px (m=1): ph0=0, dumbbell along +-x, the two lobes opposite-signed (colors are an
    # arbitrary but fixed choice of which sign is "orange" -- see level_to_rgb()/
    # orbitalLevelToColor565(); what matters physically is the opposite-sign split). ---
    mags, signs, levels = build_slice_table(2, 1, 1, extent_bohr=20.0)
    save_png('2px', signs, levels)
    _, sRight = at(mags, signs, g, mid, right)
    _, sLeft = at(mags, signs, g, mid, left)
    check('2px: +-x lobes are opposite-signed (dumbbell split)', sRight * sLeft < 0)

    # --- 2py (m=-1): ph0=pi/2, SAME dumbbell/split shape in the slice's own local x axis
    # (which sits at world azimuth pi/2) -- the physical y-dumbbell, sliced through its own
    # lobe plane instead of the world x-z plane. ---
    mags, signs, levels = build_slice_table(2, 1, -1, extent_bohr=20.0)
    save_png('2py', signs, levels)
    _, sRight = at(mags, signs, g, mid, right)
    _, sLeft = at(mags, signs, g, mid, left)
    check('2py: lobe-plane slice also shows an opposite-signed dumbbell split', sRight * sLeft < 0)

    # --- 2py sliced at ph0=0 (the WORLD x-z plane) instead of its own lobe plane: this is
    # 2py's nodal plane, so psi is ~0 everywhere on it -- this is why ph0 must be per-sign,
    # not always 0. ---
    radial_coeff = orbitals.laguerre_coeffs(2, 1)
    legendre_coeff = orbitals.legendre_coeffs(1, -1)
    max_abs = 0.0
    for row in range(g):
        z = 12.0 - (row + 0.5) * (24.0 / g)
        for col in range(g):
            x = -12.0 + (col + 0.5) * (24.0 / g)
            r = math.sqrt(x * x + z * z)
            theta = math.acos(max(-1.0, min(1.0, z / r))) if r > 1e-9 else 0.0
            # World phi is 0 on the x>0 half of the x-z plane, pi on the x<0 half --
            # psi_real() itself (not the folded-sign shortcut) makes the "this is the nodal
            # plane" point directly, so this one check bypasses build_slice_table().
            phi = 0.0 if x >= 0 else math.pi
            psi = orbitals.psi_real(r, theta, phi, 2, 1, -1, radial_coeff, legendre_coeff)
            max_abs = max(max_abs, abs(psi))
    check('2py: the x-z plane (ph0=0) is its nodal plane, psi ~= 0 everywhere', max_abs < 1e-6)

    # --- 3dxy (m=-2): ph0=pi/4, |m| even -> both halves SAME sign (same-phase lobe pair). ---
    mags, signs, levels = build_slice_table(3, 2, -2, extent_bohr=35.0)
    save_png('3dxy', signs, levels)
    _, sRight = at(mags, signs, g, mid, right)
    _, sLeft = at(mags, signs, g, mid, left)
    check('3dxy: both lobes share the same sign (|m| even, same-phase pair)', sRight * sLeft > 0)

    # --- 3dz2 (m=0): axially symmetric, no left/right sign split possible. ---
    mags, signs, levels = build_slice_table(3, 2, 0, extent_bohr=35.0)
    save_png('3dz2', signs, levels)
    _, sRight = at(mags, signs, g, mid, right)
    _, sLeft = at(mags, signs, g, mid, left)
    check('3dz2: m=0 -> left/right halves share the same sign', (sRight > 0) == (sLeft > 0))

    # --- 2s (m=0): single bright core plus a dark radial-node ring -- scanning outward from
    # the center, brightness must dip to a local minimum and then climb again, not just fall
    # off monotonically (R_2,0(r) has its one zero at r=2 bohr, see orbitals.py). ---
    mags, signs, levels = build_slice_table(2, 0, 0, extent_bohr=14.0)
    save_png('2s', signs, levels)
    row_levels = [levels[mid * g + col] for col in range(mid, g)]
    dip_col = min(range(len(row_levels)), key=lambda i: row_levels[i])
    # Robust under the density mapping: the node dip must sit below the bright core AND below
    # the outer lobe's own peak (the last cell alone is unreliable -- the tail can be as dark
    # as the node itself).
    outer_max = max(row_levels[dip_col + 1:])
    check(
        '2s: radial node -- brightness dips to an interior local min, then the outer lobe climbs above it',
        0 < dip_col < len(row_levels) - 1
        and row_levels[dip_col] < row_levels[0]
        and outer_max > row_levels[dip_col],
    )

    # --- 1s (m=0): monotonically fading blob, no sign change anywhere. ---
    mags, signs, levels = build_slice_table(1, 0, 0, extent_bohr=6.0)
    save_png('1s', signs, levels)
    check('1s: every cell shares one sign (no node, single lobe)', all(s == signs[0] for s in signs))

    print(f'\nAll checks passed. PNG previews written to {OUT_DIR}')


if __name__ == '__main__':
    main()
