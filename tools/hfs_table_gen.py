#!/usr/bin/env python3
"""Generate the shared binary data blob for the screened-potential
(HFS/atomSFE) radial tables (pc/hfs_tables_reduced.npz -- the committed
128-point-per-subshell reference, see pc/RUN_HFS.md section 5), consumed
on-demand from flash/filesystem by BOTH the ESP32 C++ port and MicroPython,
instead of being compiled into either firmware image as source-level
literals. A ~470KB module of float literals (either C++ .rodata or a Python
tuple) would cost real build time / RAM-heavy MicroPython compilation for no
benefit over reading the identical bytes from storage at the handful of
points where a (Z, n, l) lookup actually happens (once per element switch,
not once per frame or per sampled point).

Schema (see pc/hfs_tables.py's module docstring): a shared log-uniform Bohr
grid `r` (kHfsGridSize points) and, per element Z=1..92, its occupied
subshells' u(r) = r*R(r) arrays on that SAME grid (E/occ are dropped here --
unused for rendering; occupancy already comes from each port's own
electron-configuration source, slater.h/slater_data.h resp. slater.py, which
the NIST cross-check in pc/RUN_HFS.md section 5 confirms matches the table's
own configs 92/92).

Flat binary layout (little-endian, packed with `struct`):
    <HHH                gridSize, elementCount, subshellCount
    <f * gridSize        R -- shared log-uniform Bohr grid
    (<HB) * elementCount  ELEMENT[z-1] = (offset, count) into SUBSHELL/U
    (<BB) * subshellCount SUBSHELL[i] = (n, ell), i in [offset, offset+count)
    (<f * gridSize) * subshellCount   U[i], same index i, back to back

Outputs (all generated, do not edit by hand):
    data/hfs_tables.bin        for the ESP32 C++ port: staged into the
                                PlatformIO filesystem image and flashed to
                                the "storage" SPIFFS partition
                                (partitions_16M.csv) via
                                `pio run -t uploadfs`, then read on demand by
                                src/hfs_radial.cpp (hand-written) -- see that
                                file's header comment for the exact reads.
    micropython/hfs_tables.bin  byte-identical copy, deployed to the device
                                root alongside every other micropython/ file
                                (see main.py's docstring for the
                                `mpremote ... fs cp -r micropython/. :`
                                incantation) and read on demand by
                                micropython/hfs_radial_tables.py
                                (hand-written) -- see that module's
                                docstring for the exact reads.
    src/hfs_tables.h            just the three size constants above
                                (kHfsGridSize/kHfsElementCount/
                                kHfsSubshellCount) -- src/hfs_radial.cpp
                                sanity-checks the file's own header against
                                these at load time.

Embedding the data straight into flash as linked-in firmware bytes (for
MicroPython boards without a usable filesystem) is future work, see
pc/screened_potential_model.md section 7.

Usage:
    python3 tools/hfs_table_gen.py [path/to/hfs_tables_reduced.npz]
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pc'))

import hfs_tables  # noqa: E402

DEFAULT_NPZ = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'pc', 'hfs_tables_reduced.npz')
MAX_Z = 92  # display range cap (slater.MAX_DISPLAY_Z / periodic_grid.h's kMaxDisplayZ)

# Struct format strings -- the single source of truth every reader (C++ and
# MicroPython) must match byte-for-byte.
HEADER_FMT = '<HHH'      # gridSize, elementCount, subshellCount
ELEMENT_FMT = '<HB'      # offset, count
SUBSHELL_FMT = '<BB'     # n, ell


def build_flat(tables):
    """Flatten an HfsTables into (r, elements, subshells, u_rows) -- see
    module docstring for the layout. elements has exactly MAX_Z entries
    (Z=1..92, one per index z-1); subshells/u_rows are parallel, indexed by
    the running offset."""
    r = [float(x) for x in tables.r]
    elements = []
    subshells = []
    u_rows = []
    offset = 0
    for z in range(1, MAX_Z + 1):
        config = tables.config(z)
        for n, ell, _occ in config:
            src = tables.source(z, n, ell)
            subshells.append((n, ell))
            u_rows.append([float(x) for x in src.u])
        count = len(config)
        elements.append((offset, count))
        offset += count
    return r, elements, subshells, u_rows


def emit_binary(r, elements, subshells, u_rows):
    parts = [struct.pack(HEADER_FMT, len(r), len(elements), len(subshells))]
    parts.append(struct.pack('<%df' % len(r), *r))
    for offset, count in elements:
        parts.append(struct.pack(ELEMENT_FMT, offset, count))
    for n, ell in subshells:
        parts.append(struct.pack(SUBSHELL_FMT, n, ell))
    for row in u_rows:
        parts.append(struct.pack('<%df' % len(row), *row))
    return b''.join(parts)


def emit_header(grid_size, element_count, subshell_count):
    return "\n".join([
        "// Screened-potential (HFS/atomSFE) radial table SIZE CONSTANTS -- the actual",
        "// per-(Z,n,l) u(r) data lives in data/hfs_tables.bin (ESP32 SPIFFS,",
        "// `pio run -t uploadfs`) / micropython/hfs_tables.bin (MicroPython device",
        "// root), read on demand by src/hfs_radial.cpp -- see that file's header",
        "// comment for the binary format and read strategy, and this file's own",
        "// generator (tools/hfs_table_gen.py) for the schema/provenance.",
        "//",
        "// GENERATED by tools/hfs_table_gen.py from pc/hfs_tables_reduced.npz --",
        "// do not edit by hand.",
        "#pragma once",
        "",
        "constexpr int kHfsGridSize = %d;" % grid_size,
        "constexpr int kHfsElementCount = %d;" % element_count,
        "constexpr int kHfsSubshellCount = %d;" % subshell_count,
        "",
    ])


def main():
    npz_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_NPZ
    tables = hfs_tables.load(npz_path)
    assert len(tables.z_list) == MAX_Z, "expected Z=1..%d, got %d elements" % (MAX_Z, len(tables.z_list))
    r, elements, subshells, u_rows = build_flat(tables)
    blob = emit_binary(r, elements, subshells, u_rows)

    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    h_path = os.path.join(root, 'src', 'hfs_tables.h')
    data_bin_path = os.path.join(root, 'data', 'hfs_tables.bin')
    mpy_bin_path = os.path.join(root, 'micropython', 'hfs_tables.bin')

    os.makedirs(os.path.dirname(data_bin_path), exist_ok=True)
    with open(h_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(emit_header(len(r), len(elements), len(subshells)))
    with open(data_bin_path, 'wb') as f:
        f.write(blob)
    with open(mpy_bin_path, 'wb') as f:
        f.write(blob)

    print("wrote %s (%d elements, %d subshells, %d pts/subshell)" %
          (h_path, len(elements), len(subshells), len(r)))
    print("wrote %s (%d bytes)" % (data_bin_path, len(blob)))
    print("wrote %s (%d bytes)" % (mpy_bin_path, len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
