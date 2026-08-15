#!/usr/bin/env bash
# Cross-check the C++ orbital port (src/orbitals.h/.cpp + src/pointcloud.h/.cpp)
# and the MicroPython orbital port (micropython/orbitals.py +
# micropython/pointcloud.py) against the JS reference (js_reference.js,
# extracted/extended from quantum-physics.js) on the PC, before any of this
# ever runs on the ESP32. Both ports are candidates for the eventual firmware
# (C++/ESP-IDF vs MicroPython) -- this script validates correctness for both
# so that choice can be made on other grounds (performance, dev experience),
# not on "does the math even work". See README.md in this directory for what
# each tolerance pass means.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

SRC_DIR=../../src
OUT_DIR=out

mkdir -p "$OUT_DIR"

echo "== Generating JS reference artifacts (wavefunction + point cloud) =="
node gen_js_reference.js test_cases.csv "$OUT_DIR/js"
node gen_points_js.js test_cases.csv "$OUT_DIR/points_js"

echo
echo "== Building and running C++ reference (double precision) =="
g++ -std=c++17 -O2 -Wall -Wextra -DORBITAL_USE_DOUBLE \
    gen_c_reference.cpp "$SRC_DIR/orbitals.cpp" -I "$SRC_DIR" -o "$OUT_DIR/gen_c_f64"
"./$OUT_DIR/gen_c_f64" test_cases.csv "$OUT_DIR/c_f64"
g++ -std=c++17 -O2 -Wall -Wextra -DORBITAL_USE_DOUBLE \
    gen_points_c.cpp "$SRC_DIR/orbitals.cpp" "$SRC_DIR/pointcloud.cpp" -I "$SRC_DIR" -o "$OUT_DIR/gen_points_c_f64"
"./$OUT_DIR/gen_points_c_f64" test_cases.csv "$OUT_DIR/points_c_f64"

echo
echo "== Building and running C++ reference (float precision, matches ESP32 target) =="
g++ -std=c++17 -O2 -Wall -Wextra \
    gen_c_reference.cpp "$SRC_DIR/orbitals.cpp" -I "$SRC_DIR" -o "$OUT_DIR/gen_c_f32"
"./$OUT_DIR/gen_c_f32" test_cases.csv "$OUT_DIR/c_f32"
g++ -std=c++17 -O2 -Wall -Wextra \
    gen_points_c.cpp "$SRC_DIR/orbitals.cpp" "$SRC_DIR/pointcloud.cpp" -I "$SRC_DIR" -o "$OUT_DIR/gen_points_c_f32"
"./$OUT_DIR/gen_points_c_f32" test_cases.csv "$OUT_DIR/points_c_f32"

HAVE_MPY=0
if command -v micropython >/dev/null 2>&1; then
    HAVE_MPY=1
    echo
    echo "== Running MicroPython reference (micropython/orbitals.py + micropython/pointcloud.py) =="
    micropython gen_mpy_reference.py test_cases.csv "$OUT_DIR/mpy"
    micropython gen_points_mpy.py test_cases.csv "$OUT_DIR/points_mpy"
else
    echo
    echo "== Skipping MicroPython pass: 'micropython' not found on PATH =="
fi

run_pass() {
    # run_pass <label> <js_dir> <other_dir> <rtol> <atol>
    echo
    echo "== $1 =="
    set +e
    python3 compare.py "$2" "$3" --rtol "$4" --atol "$5"
    local status=$?
    set -e
    return $status
}

F64_STATUS=0
run_pass "Pass 1/6: C++ double precision wavefunction vs JS -- correctness gate for the C++ port" \
    "$OUT_DIR/js" "$OUT_DIR/c_f64" 1e-9 1e-12 || F64_STATUS=$?

F32_STATUS=0
run_pass "Pass 2/6: C++ float precision wavefunction vs JS -- informational, quantifies embedded precision loss" \
    "$OUT_DIR/js" "$OUT_DIR/c_f32" 2e-3 1e-4 || F32_STATUS=$?

POINTS_F64_STATUS=0
run_pass "Pass 3/6: C++ double precision point cloud vs JS -- correctness gate, expects bit-identical accepted points" \
    "$OUT_DIR/points_js" "$OUT_DIR/points_c_f64" 1e-9 1e-12 || POINTS_F64_STATUS=$?

POINTS_F32_STATUS=0
run_pass "Pass 4/6: C++ float precision point cloud vs JS -- informational (accept/reject boundary can differ)" \
    "$OUT_DIR/points_js" "$OUT_DIR/points_c_f32" 2e-3 1e-4 || POINTS_F32_STATUS=$?

MPY_STATUS=0
POINTS_MPY_STATUS=0
if [ "$HAVE_MPY" -eq 1 ]; then
    run_pass "Pass 5/6: MicroPython wavefunction vs JS -- correctness gate for the MicroPython port" \
        "$OUT_DIR/js" "$OUT_DIR/mpy" 1e-9 1e-12 || MPY_STATUS=$?

    run_pass "Pass 6/6: MicroPython point cloud vs JS -- correctness gate, expects bit-identical accepted points" \
        "$OUT_DIR/points_js" "$OUT_DIR/points_mpy" 1e-9 1e-12 || POINTS_MPY_STATUS=$?
fi

echo
if [ "$F64_STATUS" -eq 0 ] && [ "$POINTS_F64_STATUS" -eq 0 ]; then
    echo "RESULT: C++ double-precision port (wavefunction + point cloud) matches the JS reference. C++ port is correct."
else
    echo "RESULT: C++ double-precision port DOES NOT match the JS reference -- fix src/orbitals.cpp / src/pointcloud.cpp before trusting anything else."
fi
if [ "$F32_STATUS" -ne 0 ] || [ "$POINTS_F32_STATUS" -ne 0 ]; then
    echo "NOTE: C++ float precision diverges beyond the informational tolerance for some cases (see Pass 2/4 above) -- expected for some (n,l) combos, especially points near a rejection-sampling accept/reject boundary; review before relying on them on real hardware."
fi
if [ "$HAVE_MPY" -eq 1 ]; then
    if [ "$MPY_STATUS" -eq 0 ] && [ "$POINTS_MPY_STATUS" -eq 0 ]; then
        echo "RESULT: MicroPython port (wavefunction + point cloud) matches the JS reference. MicroPython port is correct."
    else
        echo "RESULT: MicroPython port DOES NOT match the JS reference -- fix micropython/orbitals.py / micropython/pointcloud.py before trusting anything else."
    fi
    echo "NOTE: this MicroPython pass ran on the unix port's float precision (commonly double -- see README.md), which may not match the ESP32 firmware's actual float precision. Verify on-device before using this as a precision reference for that target."
fi

if [ "$F64_STATUS" -ne 0 ] || [ "$POINTS_F64_STATUS" -ne 0 ] || \
   { [ "$HAVE_MPY" -eq 1 ] && { [ "$MPY_STATUS" -ne 0 ] || [ "$POINTS_MPY_STATUS" -ne 0 ]; }; }; then
    exit 1
fi
exit 0
