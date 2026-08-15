#!/usr/bin/env bash
# Cross-check the C++ orbital port (src/orbitals.cpp) and the MicroPython
# orbital port (micropython/orbitals.py) against the JS reference
# (js_reference.js, extracted from quantum-physics.js) on the PC, before any
# of this ever runs on the ESP32. Both ports are candidates for the eventual
# firmware (C++/ESP-IDF vs MicroPython) -- this script validates correctness
# for both so that choice can be made on other grounds (performance, dev
# experience), not on "does the math even work". See README.md in this
# directory for what each tolerance pass means.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SRC_DIR=../../src
MPY_DIR=../../micropython
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

HAVE_MPY=0
if command -v micropython >/dev/null 2>&1; then
    HAVE_MPY=1
    echo
    echo "== Running MicroPython reference (micropython/orbitals.py) =="
    micropython gen_mpy_reference.py test_cases.csv "$OUT_DIR/mpy"
else
    echo
    echo "== Skipping MicroPython pass: 'micropython' not found on PATH =="
fi

echo
echo "== Pass 1/3: C++ double precision vs JS -- correctness gate for the C++ port =="
set +e
python3 compare.py "$OUT_DIR/js" "$OUT_DIR/c_f64" --rtol 1e-9 --atol 1e-12
F64_STATUS=$?
set -e

echo
echo "== Pass 2/3: C++ float precision vs JS -- informational, quantifies embedded precision loss =="
set +e
python3 compare.py "$OUT_DIR/js" "$OUT_DIR/c_f32" --rtol 2e-3 --atol 1e-4
F32_STATUS=$?
set -e

MPY_STATUS=0
if [ "$HAVE_MPY" -eq 1 ]; then
    echo
    echo "== Pass 3/3: MicroPython vs JS -- correctness gate for the MicroPython port =="
    set +e
    python3 compare.py "$OUT_DIR/js" "$OUT_DIR/mpy" --rtol 1e-9 --atol 1e-12
    MPY_STATUS=$?
    set -e
fi

echo
if [ "$F64_STATUS" -eq 0 ]; then
    echo "RESULT: C++ double-precision port matches the JS reference. C++ port is correct."
else
    echo "RESULT: C++ double-precision port DOES NOT match the JS reference -- fix src/orbitals.cpp before trusting anything else."
fi
if [ "$F32_STATUS" -ne 0 ]; then
    echo "NOTE: C++ float precision diverges beyond the informational tolerance for some cases (see Pass 2 above) -- expected for some (n,l) combos, review before relying on them on real hardware."
fi
if [ "$HAVE_MPY" -eq 1 ]; then
    if [ "$MPY_STATUS" -eq 0 ]; then
        echo "RESULT: MicroPython port matches the JS reference. MicroPython port is correct."
    else
        echo "RESULT: MicroPython port DOES NOT match the JS reference -- fix micropython/orbitals.py before trusting anything else."
    fi
    echo "NOTE: this MicroPython pass ran on the unix port's float precision (commonly double -- see README.md), which may not match the ESP32 firmware's actual float precision. Verify on-device before using this as a precision reference for that target."
fi

if [ "$F64_STATUS" -ne 0 ] || { [ "$HAVE_MPY" -eq 1 ] && [ "$MPY_STATUS" -ne 0 ]; }; then
    exit 1
fi
exit 0
