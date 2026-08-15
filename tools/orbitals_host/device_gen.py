"""Runs ON the ESP32-S3 itself (not the unix port): generates the same
wavefunction + point-cloud CSV artifacts as gen_mpy_reference.py /
gen_points_mpy.py, writing them as real files under /out_dev/ on the
device's flash. (An earlier version streamed CSV rows directly over the
serial console via `mpremote run`, but printing ~20k lines that way got
interleaved/corrupted -- likely serial buffering under sustained load, not a
script bug. Writing files and pulling them back with `mpremote fs cp -r`,
which uses a chunked/acknowledged transfer protocol, is reliable instead.)

See run_on_device.sh, which copies this alongside micropython/orbitals.py,
micropython/pointcloud.py and test_cases.csv, runs it, then pulls
/out_dev/ back to reconstruct the usual per-case CSV files for compare.py.
"""
import math
import os

import orbitals
import pointcloud

TABLE_SIZE = 1001
SEED = 12345
POINTS_PER_CASE = 100
R_FRACS = (0.0, 0.02, 0.05, 0.1, 0.2, 0.35, 0.55, 0.8)
THETA_MULS = (0, 1, 2, 3, 4, 5, 6)
PHI_FRACS = (0, 0.25, 0.5, 1.0, 1.5)

OUT_DIR = '/out_dev'


def fmt(v):
    return '%.17g' % v


def read_cases():
    cases = []
    with open('test_cases.csv') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            n, l, m = [int(x) for x in line.split(',')]
            cases.append((n, l, m))
    return cases


def write_file(name, lines):
    with open(OUT_DIR + '/' + name, 'w') as f:
        f.write('\n'.join(lines) + '\n')


try:
    os.mkdir(OUT_DIR)
except OSError:
    pass  # already exists

for n, l, m in read_cases():
    p = '{}_{}_{}'.format(n, l, m)

    legendre_coeff = orbitals.legendre_coeffs(l, m)
    laguerre_coeff = orbitals.laguerre_coeffs(n, l)

    coeff_lines = ['kind,index,value']
    for i, v in enumerate(legendre_coeff):
        coeff_lines.append('legendre,{},{}'.format(i, fmt(v)))
    degree = n - l if n - l > 0 else 1
    for i in range(degree):
        coeff_lines.append('laguerre,{},{}'.format(i, fmt(laguerre_coeff[i])))
    write_file(p + '_coeffs.csv', coeff_lines)

    legendre_table = orbitals.build_legendre_table(l, m, TABLE_SIZE)
    lg_lines = ['i,theta,value']
    for i in range(TABLE_SIZE):
        theta = math.pi * i / (TABLE_SIZE - 1)
        lg_lines.append('{},{},{}'.format(i, fmt(theta), fmt(legendre_table[i])))
    write_file(p + '_legendre_table.csv', lg_lines)

    radial_table, max_r = orbitals.build_radial_table(n, l, TABLE_SIZE)
    delta_r = max_r / (TABLE_SIZE - 1)
    rad_lines = ['# maxR={}'.format(fmt(max_r)), 'i,r,value']
    for i in range(TABLE_SIZE):
        rad_lines.append('{},{},{}'.format(i, fmt(i * delta_r), fmt(radial_table[i])))
    write_file(p + '_radial_table.csv', rad_lines)

    psi_lines = ['r_frac,theta,phi,psi']
    for rf in R_FRACS:
        r = rf * max_r
        for tm in THETA_MULS:
            theta = math.pi * tm / 6
            for pf in PHI_FRACS:
                phi = math.pi * pf
                psi = orbitals.psi_real(r, theta, phi, n, l, m, laguerre_coeff, legendre_coeff)
                psi_lines.append('{},{},{},{}'.format(fmt(rf), fmt(theta), fmt(phi), fmt(psi)))
    write_file(p + '_psi_samples.csv', psi_lines)

    sampler = pointcloud.init_orbital_sampler(n, l, m)
    rng = pointcloud.XorShift32(SEED)
    pt_lines = ['index,x,y,z']
    for i in range(POINTS_PER_CASE):
        x, y, z = pointcloud.sample_orbital_point(sampler, rng)
        pt_lines.append('{},{},{},{}'.format(i, fmt(x), fmt(y), fmt(z)))
    write_file(p + '_points.csv', pt_lines)

    print('PROGRESS:' + p)

print('DONE')
