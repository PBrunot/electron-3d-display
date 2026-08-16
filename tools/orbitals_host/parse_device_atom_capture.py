"""Extracts the ATOMTEST,... tagged CSV lines from a raw serial capture of the
device running main.cpp's ATOM_VALIDATION_TEST build (see that file and
gen_atom_reference.py) and reconstructs the same per-element CSV files
gen_atom_reference.py writes, so compare_atom.py can diff device vs host
uniformly.

The capture will have other log lines mixed in (boot messages, ESP_LOGI's
"I (12345) tag:" prefix, etc.) -- this just greps for the "ATOMTEST," marker
anywhere in each line and parses from there, ignoring everything before it.

Usage:
    python3 parse_device_atom_capture.py <capture.log> <out_dir>

Capture the log with e.g.:
    pio device monitor > capture.log
(uncomment ATOM_VALIDATION_TEST in main.cpp, build+flash first; stop the
monitor once "ATOMTEST,DONE" appears).
"""
import sys


def main():
    if len(sys.argv) != 3:
        print('Usage: python3 parse_device_atom_capture.py <capture.log> <out_dir>')
        sys.exit(1)
    capture_path, out_dir = sys.argv[1], sys.argv[2]

    import os
    try:
        os.mkdir(out_dir)
    except OSError:
        pass

    config_rows = {}  # symbol -> [lines]
    zeff_rows = {}
    point_rows = {}
    saw_done = False

    with open(capture_path, errors='replace') as f:
        for raw_line in f:
            idx = raw_line.find('ATOMTEST,')
            if idx < 0:
                continue
            line = raw_line[idx:].strip()
            parts = line.split(',')
            kind = parts[1] if len(parts) > 1 else ''

            if kind == 'DONE':
                saw_done = True
            elif kind == 'CONFIG':
                _, _, symbol, n, ell, occ = parts
                config_rows.setdefault(symbol, ['n,ell,occ']).append('%s,%s,%s' % (n, ell, occ))
            elif kind == 'ZEFF':
                _, _, symbol, n, ell, zeff = parts
                zeff_rows.setdefault(symbol, ['n,ell,zeff']).append('%s,%s,%s' % (n, ell, zeff))
            elif kind == 'POINT':
                _, _, symbol, index, x, y, z = parts
                point_rows.setdefault(symbol, ['index,x,y,z']).append('%s,%s,%s,%s' % (index, x, y, z))

    if not saw_done:
        print('WARNING: no ATOMTEST,DONE marker found -- capture may be incomplete.')

    symbols = sorted(set(config_rows) | set(zeff_rows) | set(point_rows))
    if not symbols:
        print('ERROR: no ATOMTEST lines found in %s -- did you build with ATOM_VALIDATION_TEST '
              'defined and capture the right log?' % capture_path)
        sys.exit(1)

    for symbol in symbols:
        if symbol in config_rows:
            with open('%s/%s_config.csv' % (out_dir, symbol), 'w') as f:
                f.write('\n'.join(config_rows[symbol]) + '\n')
        if symbol in zeff_rows:
            with open('%s/%s_zeff.csv' % (out_dir, symbol), 'w') as f:
                f.write('\n'.join(zeff_rows[symbol]) + '\n')
        if symbol in point_rows:
            with open('%s/%s_points.csv' % (out_dir, symbol), 'w') as f:
                f.write('\n'.join(point_rows[symbol]) + '\n')

    print('Parsed %d elements from %s -> %s' % (len(symbols), capture_path, out_dir))


main()
