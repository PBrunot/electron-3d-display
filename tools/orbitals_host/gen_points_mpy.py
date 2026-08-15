"""Generate MicroPython-reference point-cloud CSV artifacts for the
hydrogen-orbital rejection sampler, using micropython/pointcloud.py.

Usage (run under the MicroPython unix port, not CPython):
    micropython gen_points_mpy.py <test_cases.csv> <out_dir>
"""
import sys

sys.path.append('../../micropython')
import pointcloud  # noqa: E402  (import after sys.path tweak, must precede use)

# MUST match the constants in gen_points_c.cpp and gen_points_js.js exactly.
SEED = 12345
POINTS_PER_CASE = 100


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


def prefix(n, l, m):
    return '{}_{}_{}'.format(n, l, m)


def main():
    if len(sys.argv) != 3:
        print('Usage: micropython gen_points_mpy.py <test_cases.csv> <out_dir>')
        sys.exit(1)
    test_cases_path = sys.argv[1]
    out_dir = sys.argv[2]
    mkdir_p(out_dir)

    for n, l, m in parse_test_cases(test_cases_path):
        p = prefix(n, l, m)
        sampler = pointcloud.init_orbital_sampler(n, l, m)
        rng = pointcloud.XorShift32(SEED)

        with open(out_dir + '/' + p + '_points.csv', 'w') as f:
            f.write('index,x,y,z\n')
            for i in range(POINTS_PER_CASE):
                x, y, z = pointcloud.sample_orbital_point(sampler, rng)
                f.write('{},{},{},{}\n'.format(i, fmt(x), fmt(y), fmt(z)))

        print('MicroPython: wrote {} points for n={} l={} m={}'.format(POINTS_PER_CASE, n, l, m))


main()
