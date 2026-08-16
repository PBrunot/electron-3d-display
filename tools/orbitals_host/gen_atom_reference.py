"""Host-side reference generator for validating src/slater.h + src/atom_cloud.h
(the ESP32 C++ atom point-cloud port) against the already cross-validated
MicroPython implementation (micropython/slater.py + micropython/atom_cloud.py).

Must run under the real `micropython` unix-port binary, not plain CPython --
atom_cloud.py/pointcloud.py/orbitals.py use @micropython.native, which is a
compiler directive recognized only by the real interpreter (see
tools/orbitals_host/README.md's "Ottimizzazioni MicroPython" section).

Usage:
    micropython gen_atom_reference.py <out_dir>

For each element in ATOM_TEST_CASES, writes three CSV files matching the
three validation passes in ATOMS.md's validation plan:
    <symbol>_config.csv  -- n,ell,occ (exact match expected, no tolerance)
    <symbol>_zeff.csv    -- n,ell,zeff (float, rtol~2e-3 expected)
    <symbol>_points.csv  -- index,x,y,z for POINTS_PER_CASE points (float,
                            rtol~2e-3 expected) -- same seed/count as the
                            device capture, so per-point order and content
                            should match if src/atom_cloud.h's drawing-group
                            order and Hund filling are correct.
"""
import os
import sys

sys.path.append('../../micropython')
import atom_cloud  # noqa: E402  (import after sys.path tweak, must precede use)
import slater  # noqa: E402

# MUST match the device validation build's constants exactly (main.cpp's
# ATOM_VALIDATION_TEST block) -- same test elements, same seed, same point count.
ATOM_TEST_CASES = (1, 2, 6, 10, 24, 26, 46, 58)  # H, He, C, Ne, Cr, Fe, Pd, Ce
SEED = 12345
POINTS_PER_CASE = 50


def fmt(v):
    return '%.17g' % v


def write_file(out_dir, name, lines):
    with open(out_dir + '/' + name, 'w') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    if len(sys.argv) != 2:
        print('Usage: micropython gen_atom_reference.py <out_dir>')
        sys.exit(1)
    out_dir = sys.argv[1]
    try:
        os.mkdir(out_dir)
    except OSError:
        pass  # already exists

    for z in ATOM_TEST_CASES:
        symbol = slater.element_symbol(z)
        config = slater.electron_configuration(z)

        config_lines = ['n,ell,occ']
        for n, ell, occ in config:
            config_lines.append('%d,%d,%d' % (n, ell, occ))
        write_file(out_dir, '%s_config.csv' % symbol, config_lines)

        zeff_lines = ['n,ell,zeff']
        for n, ell, occ in config:
            z_eff = slater.z_eff_radial(z, config, n, ell)
            zeff_lines.append('%d,%d,%s' % (n, ell, fmt(z_eff)))
        write_file(out_dir, '%s_zeff.csv' % symbol, zeff_lines)

        xs, ys, zs, _colors, _shells, _ells, _signs, _config = atom_cloud.build_atom_point_cloud(
            z, count=POINTS_PER_CASE, seed=SEED)
        point_lines = ['index,x,y,z']
        for i in range(POINTS_PER_CASE):
            point_lines.append('%d,%s,%s,%s' % (i, fmt(xs[i]), fmt(ys[i]), fmt(zs[i])))
        write_file(out_dir, '%s_points.csv' % symbol, point_lines)

        print('PROGRESS:%s (Z=%d)' % (symbol, z))

    print('DONE')


main()
