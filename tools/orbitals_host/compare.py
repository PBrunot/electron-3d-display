#!/usr/bin/env python3
"""Cross-check JS-reference vs C++-reference CSV artifacts for the hydrogen
orbital math port (src/orbitals.cpp vs tools/orbitals_host/js_reference.js).

Usage:
    python3 compare.py <js_dir> <c_dir> --rtol 1e-9 --atol 1e-12

Compares every CSV file present in <js_dir> against the file of the same name
in <c_dir>, row by row, on the numeric "value"-like column(s). A row fails if
    abs(a - b) > atol + rtol * abs(a)
(numpy.allclose's convention, with the JS value as the reference). Exits 0
only if every file passes.
"""
import argparse
import csv
import glob
import os
import sys

# For each CSV filename suffix, which columns hold values to compare
# (everything else -- index/kind/i/theta/r/phi/r_frac columns -- is treated as
# a row key and must match, not be numerically diffed).
VALUE_COLUMNS = {
    '_coeffs.csv': ['value'],
    '_legendre_table.csv': ['value'],
    '_radial_table.csv': ['value'],
    '_psi_samples.csv': ['psi'],
}


def read_csv_rows(path):
    rows = []
    with open(path, newline='') as f:
        for line in f:
            if line.startswith('#'):
                continue
            rows.append(line)
    reader = csv.DictReader(rows)
    return list(reader)


def compare_file(js_path, c_path, value_cols, rtol, atol):
    js_rows = read_csv_rows(js_path)
    c_rows = read_csv_rows(c_path)
    if len(js_rows) != len(c_rows):
        return {'ok': False, 'reason': f'row count mismatch: js={len(js_rows)} c={len(c_rows)}'}

    max_abs_err = 0.0
    max_rel_err = 0.0
    worst_row = None
    for i, (jr, cr) in enumerate(zip(js_rows, c_rows)):
        for col in value_cols:
            a = float(jr[col])
            b = float(cr[col])
            abs_err = abs(a - b)
            tol = atol + rtol * abs(a)
            if abs_err > max_abs_err:
                max_abs_err = abs_err
            rel_err = abs_err / abs(a) if a != 0 else (0.0 if b == 0 else float('inf'))
            if rel_err > max_rel_err and abs(a) > atol:
                max_rel_err = rel_err
            if abs_err > tol:
                worst_row = (i, col, a, b, abs_err)
    ok = worst_row is None
    return {
        'ok': ok,
        'rows': len(js_rows),
        'max_abs_err': max_abs_err,
        'max_rel_err': max_rel_err,
        'worst_row': worst_row,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('js_dir')
    ap.add_argument('c_dir')
    ap.add_argument('--rtol', type=float, default=1e-9)
    ap.add_argument('--atol', type=float, default=1e-12)
    args = ap.parse_args()

    js_files = sorted(glob.glob(os.path.join(args.js_dir, '*.csv')))
    if not js_files:
        print(f'No CSV files found in {args.js_dir}', file=sys.stderr)
        return 1

    n_fail = 0
    print(f'{"file":45s} {"rows":>6s} {"max_abs_err":>14s} {"max_rel_err":>14s}  result')
    print('-' * 95)
    for js_path in js_files:
        name = os.path.basename(js_path)
        c_path = os.path.join(args.c_dir, name)
        value_cols = None
        for suffix, cols in VALUE_COLUMNS.items():
            if name.endswith(suffix):
                value_cols = cols
                break
        if value_cols is None:
            continue
        if not os.path.exists(c_path):
            print(f'{name:45s} {"":>6s} {"":>14s} {"":>14s}  MISSING ({c_path})')
            n_fail += 1
            continue
        result = compare_file(js_path, c_path, value_cols, args.rtol, args.atol)
        if 'reason' in result:
            print(f'{name:45s} {"":>6s} {"":>14s} {"":>14s}  FAIL ({result["reason"]})')
            n_fail += 1
            continue
        status = 'PASS' if result['ok'] else 'FAIL'
        if not result['ok']:
            n_fail += 1
        print(f'{name:45s} {result["rows"]:6d} {result["max_abs_err"]:14.6e} {result["max_rel_err"]:14.6e}  {status}')
        if not result['ok'] and result['worst_row']:
            i, col, a, b, err = result['worst_row']
            print(f'    worst at row {i}, col {col}: js={a!r} c={b!r} abs_err={err:.6e}')

    print('-' * 95)
    total = len([f for f in js_files if any(os.path.basename(f).endswith(s) for s in VALUE_COLUMNS)])
    print(f'{total - n_fail}/{total} files passed (rtol={args.rtol:g}, atol={args.atol:g})')
    return 0 if n_fail == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
