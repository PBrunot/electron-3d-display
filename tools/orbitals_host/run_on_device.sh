#!/usr/bin/env bash
# Run the MicroPython orbital math + point-cloud port on a REAL, connected
# ESP32-S3 (not the unix port used by run_crosscheck.sh) and cross-check its
# output against the JS reference. This is the actual target hardware for
# the "MicroPython on ESP32" option in the ESP-IDF/C++ vs MicroPython
# decision -- run_crosscheck.sh's unix-port MicroPython pass only tells you
# the *algorithm* is ported correctly; this script additionally tells you
# what the real board's float precision and rejection-sampling performance
# actually are, since the unix port's double-precision floats are not
# representative of the on-device build (see README.md).
#
# Requires: an ESP32-S3 already running MicroPython, reachable over serial
# (flash it first with esptool + the official ESP32_GENERIC_S3-SPIRAM_OCT
# firmware from https://micropython.org/download/ESP32_GENERIC_S3/ if it
# isn't already), and `mpremote` on PATH (`pip3 install --user mpremote`).
#
# Usage:
#   ./run_on_device.sh [serial-port]
# Defaults to /dev/ttyACM0; override via the MPY_PORT env var or first arg.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

PORT="${1:-${MPY_PORT:-/dev/ttyACM0}}"
MPY_DIR=../../micropython
OUT_DIR=out

if ! command -v mpremote >/dev/null 2>&1; then
    echo "mpremote not found on PATH. Install with: pip3 install --user mpremote" >&2
    exit 1
fi

if [ ! -f "$OUT_DIR/js/1_0_0_radial_table.csv" ] || [ ! -f "$OUT_DIR/points_js/1_0_0_points.csv" ]; then
    echo "== JS reference artifacts not found in $OUT_DIR -- generating them first (see run_crosscheck.sh) =="
    node gen_js_reference.js test_cases.csv "$OUT_DIR/js"
    node gen_points_js.js test_cases.csv "$OUT_DIR/points_js"
fi

echo "== Copying micropython/orbitals.py, micropython/pointcloud.py, test_cases.csv to $PORT =="
mpremote connect "$PORT" cp "$MPY_DIR/orbitals.py" :orbitals.py
mpremote connect "$PORT" cp "$MPY_DIR/pointcloud.py" :pointcloud.py
mpremote connect "$PORT" cp test_cases.csv :test_cases.csv

echo
echo "== Running device_gen.py on-device (11 cases: tables + psi samples + 100-point rejection sampling each) =="
echo "   Writes CSVs to /out_dev/ on the device's flash (a first version streamed rows directly over the"
echo "   serial console via 'mpremote run', but printing ~20k lines that way got interleaved/corrupted --"
echo "   likely serial buffering under sustained load). This takes roughly 1-2 minutes; the high-(n,l)"
echo "   cases are the slow part (low rejection-sampling acceptance rate x interpreted-MicroPython"
echo "   overhead), not the table builds."
mkdir -p "$OUT_DIR"
mpremote connect "$PORT" run device_gen.py

echo
echo "== Pulling /out_dev/ back from the device =="
echo "   ('mpremote fs cp -r' hit what looks like an mpremote bug on this build -- it raised"
echo "   IsADirectoryError partway through, i.e. treated the recursive copy as non-recursive for some"
echo "   entries -- so instead we cp each of the 55 known files individually, chained via '+' into one"
echo "   mpremote session so it doesn't reconnect/reset per file. Takes ~2 minutes for ~230KB total.)"
rm -rf "$OUT_DIR/device_pulled"
mkdir -p "$OUT_DIR/device_pulled"
mapfile -t PULL_ARGS < <(python3 -c "
suffixes = ['coeffs.csv', 'legendre_table.csv', 'radial_table.csv', 'psi_samples.csv', 'points.csv']
with open('test_cases.csv') as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        n, l, m = line.split(',')
        prefix = '{}_{}_{}'.format(n, l, m)
        for suf in suffixes:
            name = '{}_{}'.format(prefix, suf)
            print('fs')
            print('cp')
            print(':out_dev/' + name)
            print('$OUT_DIR/device_pulled/' + name)
            print('+')
")
unset 'PULL_ARGS[${#PULL_ARGS[@]}-1]' # drop trailing '+'
mpremote connect "$PORT" "${PULL_ARGS[@]}"

echo
echo "== Splitting pulled files into $OUT_DIR/esp32/ and $OUT_DIR/points_esp32/ =="
python3 parse_device_output.py "$OUT_DIR/device_pulled" "$OUT_DIR/esp32" "$OUT_DIR/points_esp32"

# The on-device build uses single-precision floats (see README.md), so it is
# compared at the same informational tolerance as the C++ float32 build --
# this is not expected to be a bit-exact match to the JS/double reference.
echo
echo "== Wavefunction: ESP32-S3 (float32) vs JS reference (double) -- informational, same tolerance as the C++ float32 pass =="
set +e
python3 compare.py "$OUT_DIR/js" "$OUT_DIR/esp32" --rtol 2e-3 --atol 1e-4
WAVE_STATUS=$?
set -e

echo
echo "== Point cloud: ESP32-S3 (float32) vs JS reference (double) -- informational =="
set +e
python3 compare.py "$OUT_DIR/points_js" "$OUT_DIR/points_esp32" --rtol 2e-3 --atol 1e-4
POINTS_STATUS=$?
set -e

echo
if [ "$WAVE_STATUS" -eq 0 ] && [ "$POINTS_STATUS" -eq 0 ]; then
    echo "RESULT: real ESP32-S3 output matches the JS/double reference within float32 tolerance for every case."
else
    echo "RESULT: real ESP32-S3 output diverges beyond the informational tolerance for some cases (see above) -- inspect before trusting those specific (n,l,m) combos on this hardware."
fi
echo "NOTE: this is informational, not a correctness gate -- unlike run_crosscheck.sh's unix-port pass, a" \
     "mismatch here does not necessarily mean micropython/orbitals.py or pointcloud.py has a bug; it may" \
     "just be float32 cancellation (see the known 9_7_3_psi_samples.csv near-zero-crossing case)."
