"""Generate MicroPython-reference CSV artifacts for the hydrogen-orbital math
cross-check, using micropython/orbitals.py -- a candidate implementation for
running this project's orbital math directly as MicroPython on the ESP32,
evaluated here as an alternative to the C++/ESP-IDF port in src/physics/orbitals.h/.cpp.

Usage (run under the MicroPython unix port, not CPython):
    micropython gen_mpy_reference.py <test_cases.csv> <out_dir>

Written against MicroPython's stdlib subset (no argparse, no csv module,
no os.makedirs) so it runs as-is under `micropython`; it also happens to run
unchanged under CPython 3 since it avoids anything MicroPython-specific.
"""
import math
import sys

sys.path.append('../../micropython')
import orbitals  # noqa: E402  (import after sys.path tweak, must precede use)

TABLE_SIZE = 1001

# Sample grid for psi_samples.csv -- MUST match the constants in
# gen_js_reference.js and gen_c_reference.cpp exactly (r as a fraction of
# maxR, theta/phi in radians).
R_FRACS = [0.0, 0.02, 0.05, 0.1, 0.2, 0.35, 0.55, 0.8]
THETA_MULS = [0, 1, 2, 3, 4, 5, 6]  # multiplied by pi/6 below
PHI_FRACS = [0, 0.25, 0.5, 1.0, 1.5]  # multiplied by pi below


def fmt(v):
    return '%.17g' % v


def parse_test_cases(path):
    cases = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            n, l, m = [int(tok) for tok in line.split(',')]
            cases.append((n, l, m))
    return cases


def mkdir_p(path):
    parts = path.split('/')
    accum = ''
    for part in parts:
        if part == '':
            continue
        accum = accum + '/' + part if accum else part
        try:
            import os
            os.mkdir(accum)
        except OSError:
            pass  # already exists (or unwritable, which will fail loudly on file write below)


def write_csv(path, header, rows):
    with open(path, 'w') as f:
        f.write(','.join(header) + '\n')
        for row in rows:
            f.write(','.join(fmt(v) if isinstance(v, float) else str(v) for v in row) + '\n')


def prefix(n, l, m):
    return '{}_{}_{}'.format(n, l, m)


def main():
    if len(sys.argv) != 3:
        print('Usage: micropython gen_mpy_reference.py <test_cases.csv> <out_dir>')
        sys.exit(1)
    test_cases_path = sys.argv[1]
    out_dir = sys.argv[2]
    mkdir_p(out_dir)

    for n, l, m in parse_test_cases(test_cases_path):
        p = prefix(n, l, m)

        legendre_coeff = orbitals.legendre_coeffs(l, m)
        laguerre_coeff = orbitals.laguerre_coeffs(n, l)

        # coeffs.csv
        coeff_rows = []
        for i, v in enumerate(legendre_coeff):
            coeff_rows.append(('legendre', i, v))
        degree = n - l if n - l > 0 else 1
        for i in range(degree):
            coeff_rows.append(('laguerre', i, laguerre_coeff[i]))
        write_csv(out_dir + '/' + p + '_coeffs.csv', ['kind', 'index', 'value'], coeff_rows)

        # legendre_table.csv
        legendre_table = orbitals.build_legendre_table(l, m, TABLE_SIZE)
        legendre_rows = [(i, math.pi * i / (TABLE_SIZE - 1), legendre_table[i]) for i in range(TABLE_SIZE)]
        write_csv(out_dir + '/' + p + '_legendre_table.csv', ['i', 'theta', 'value'], legendre_rows)

        # radial_table.csv
        radial_table, max_r = orbitals.build_radial_table(n, l, TABLE_SIZE)
        delta_r = max_r / (TABLE_SIZE - 1)
        with open(out_dir + '/' + p + '_radial_table.csv', 'w') as f:
            f.write('# maxR={}\n'.format(fmt(max_r)))
            f.write('i,r,value\n')
            for i in range(TABLE_SIZE):
                f.write('{},{},{}\n'.format(i, fmt(i * delta_r), fmt(radial_table[i])))

        # psi_samples.csv
        psi_rows = []
        for rf in R_FRACS:
            r = rf * max_r
            for theta_mul in THETA_MULS:
                theta = math.pi * theta_mul / 6
                for phi_frac in PHI_FRACS:
                    phi = math.pi * phi_frac
                    psi = orbitals.psi_real(r, theta, phi, n, l, m, laguerre_coeff, legendre_coeff)
                    psi_rows.append((rf, theta, phi, psi))
        write_csv(out_dir + '/' + p + '_psi_samples.csv', ['r_frac', 'theta', 'phi', 'psi'], psi_rows)

        print('MicroPython: wrote artifacts for n={} l={} m={}'.format(n, l, m))


main()
