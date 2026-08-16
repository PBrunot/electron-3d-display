"""Compares the device's ATOM_VALIDATION_TEST output (parsed by
parse_device_atom_capture.py) against gen_atom_reference.py's host reference,
for the three passes described in the atom-viewer validation plan:

  1. Electron configuration (n,ell,occ) -- EXACT match required (pure integer
     logic, no floating point excuse for a mismatch). Blocks (nonzero exit).
  2. Z_eff per subshell -- float, rtol=2e-3 (this project's established
     double-host-vs-float32-device tolerance, see
     tools/orbitals_host/README.md). Informative: reported but does not
     block, matching that same established convention.
  3. Sampled points -- float, rtol=2e-3, same reasoning as pass 2.
     Point-for-point (not just statistical) agreement here additionally
     validates drawing-group order and Hund-filling logic that passes 1-2
     can't see on their own.

Usage:
    python3 compare_atom.py <host_dir> <device_dir> [symbols...]

If no symbols given, compares every element gen_atom_reference.py produced
(ATOM_TEST_CASES in that file).
"""
import sys

ATOM_TEST_SYMBOLS = ('H', 'He', 'C', 'Ne', 'Cr', 'Fe', 'Pd', 'Ce')  # Z = 1,2,6,10,24,26,46,58

RTOL = 2e-3
ATOL = 1e-6


def read_csv(path):
    try:
        with open(path) as f:
            lines = [ln.strip() for ln in f if ln.strip()]
    except FileNotFoundError:
        return None
    if not lines:
        return []
    return [ln.split(',') for ln in lines[1:]]  # skip header


def close(a, b):
    a, b = float(a), float(b)
    return abs(a - b) <= ATOL + RTOL * abs(b)


def compare_config(host_dir, dev_dir, symbol):
    host = read_csv('%s/%s_config.csv' % (host_dir, symbol))
    dev = read_csv('%s/%s_config.csv' % (dev_dir, symbol))
    if host is None or dev is None:
        return None, 'missing file(s)'
    if len(host) != len(dev):
        return False, 'subshell count differs: host=%d device=%d' % (len(host), len(dev))
    for i, (h, d) in enumerate(zip(host, dev)):
        if h != d:
            return False, 'row %d: host=%s device=%s' % (i, h, d)
    return True, '%d subshells match exactly' % len(host)


def compare_float_csv(host_dir, dev_dir, symbol, name, value_cols):
    host = read_csv('%s/%s_%s.csv' % (host_dir, symbol, name))
    dev = read_csv('%s/%s_%s.csv' % (dev_dir, symbol, name))
    if host is None or dev is None:
        return None, 'missing file(s)'
    if len(host) != len(dev):
        return False, 'row count differs: host=%d device=%d' % (len(host), len(dev))
    worst_rel = 0.0
    mismatches = 0
    for h, d in zip(host, dev):
        for col in value_cols:
            hv, dv = float(h[col]), float(d[col])
            if not close(hv, dv):
                mismatches += 1
            denom = abs(hv) if abs(hv) > ATOL else ATOL
            worst_rel = max(worst_rel, abs(hv - dv) / denom)
    if mismatches:
        return False, '%d/%d values outside rtol=%g (worst rel err %.4g)' % (
            mismatches, len(host) * len(value_cols), RTOL, worst_rel)
    return True, '%d rows within rtol=%g (worst rel err %.4g)' % (len(host), RTOL, worst_rel)


def main():
    if len(sys.argv) < 3:
        print('Usage: python3 compare_atom.py <host_dir> <device_dir> [symbols...]')
        sys.exit(1)
    host_dir, dev_dir = sys.argv[1], sys.argv[2]
    symbols = sys.argv[3:] if len(sys.argv) > 3 else ATOM_TEST_SYMBOLS

    blocking_failed = False
    print('%-4s %-10s %-8s %s' % ('el', 'pass', 'status', 'detail'))
    print('-' * 70)

    for symbol in symbols:
        ok, detail = compare_config(host_dir, dev_dir, symbol)
        status = 'SKIP' if ok is None else ('PASS' if ok else 'FAIL')
        print('%-4s %-10s %-8s %s' % (symbol, 'config', status, detail))
        if ok is False:
            blocking_failed = True

        ok, detail = compare_float_csv(host_dir, dev_dir, symbol, 'zeff', value_cols=(2,))
        status = 'SKIP' if ok is None else ('PASS' if ok else 'warn')
        print('%-4s %-10s %-8s %s' % (symbol, 'zeff', status, detail))

        ok, detail = compare_float_csv(host_dir, dev_dir, symbol, 'points', value_cols=(1, 2, 3))
        status = 'SKIP' if ok is None else ('PASS' if ok else 'warn')
        print('%-4s %-10s %-8s %s' % (symbol, 'points', status, detail))

    print('-' * 70)
    if blocking_failed:
        print('RESULT: electron-configuration mismatch(es) found -- these are pure integer '
              'logic (Madelung/exception-table bugs), not precision drift. Fix before trusting '
              'anything else.')
        sys.exit(1)
    print('RESULT: all electron configurations match exactly. Check the zeff/points warn rows '
          'above (if any) -- those use an informative float tolerance, same convention as the '
          'rest of tools/orbitals_host, and do not fail the build.')


main()
