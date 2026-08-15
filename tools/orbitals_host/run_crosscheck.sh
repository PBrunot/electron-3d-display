#!/usr/bin/env bash
# Cross-check the C++ hydrogen-orbital port (src/orbitals.cpp) against the JS
# reference (js_reference.js, extracted from quantum-physics.js) on the PC,
# before any of this ever runs on the ESP32. See README.md in this directory
# for what the two tolerance passes mean.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SRC_DIR=../../src
OUT_DIR=out

mkdir -p "$OUT_DIR"

echo "== Generating JS reference artifacts =="
node gen_js_reference.js test_cases.csv "$OUT_DIR/js"

echo
echo "== Building and running C++ reference (double precision) =="
g++ -std=c++17 -O2 -Wall -Wextra -DORBITAL_USE_DOUBLE \
    gen_c_reference.cpp "$SRC_DIR/orbitals.cpp" -I "$SRC_DIR" -o "$OUT_DIR/gen_c_f64"
"./$OUT_DIR/gen_c_f64" test_cases.csv "$OUT_DIR/c_f64"

echo
echo "== Building and running C++ reference (float precision, matches ESP32 target) =="
g++ -std=c++17 -O2 -Wall -Wextra \
    gen_c_reference.cpp "$SRC_DIR/orbitals.cpp" -I "$SRC_DIR" -o "$OUT_DIR/gen_c_f32"
"./$OUT_DIR/gen_c_f32" test_cases.csv "$OUT_DIR/c_f32"

echo
echo "== Pass 1/2: double precision vs JS -- correctness gate for the port =="
set +e
python3 compare.py "$OUT_DIR/js" "$OUT_DIR/c_f64" --rtol 1e-9 --atol 1e-12
F64_STATUS=$?
set -e

echo
echo "== Pass 2/2: float precision vs JS -- informational, quantifies embedded precision loss =="
set +e
python3 compare.py "$OUT_DIR/js" "$OUT_DIR/c_f32" --rtol 2e-3 --atol 1e-4
F32_STATUS=$?
set -e

echo
if [ "$F64_STATUS" -eq 0 ]; then
    echo "RESULT: double-precision port matches the JS reference. C++ port is correct."
else
    echo "RESULT: double-precision port DOES NOT match the JS reference -- fix src/orbitals.cpp before trusting anything else."
fi
if [ "$F32_STATUS" -ne 0 ]; then
    echo "NOTE: float precision diverges beyond the informational tolerance for some cases (see Pass 2 above) -- expected for some (n,l) combos, review before relying on them on real hardware."
fi

exit "$F64_STATUS"
