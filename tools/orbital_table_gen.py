#!/usr/bin/env python3
"""Generate the flat binary data blob for orbital_library.h's kOrbitalLibrary
(the 36 fixed hydrogen-orbital presets, 1s..6pz): each preset's
OrbitalSampler, consumed ON DEMAND from flash/filesystem by
src/orbital_library.cpp instead of being compiled into firmware as a
~423KB .rodata table for data that's only ever needed a preset at a time
(see src/orbital_library.cpp's header comment).

Mirrors tools/hfs_table_gen.py's data/logic split (see that file's header
comment and src/hfs_radial.h's) -- this generator reuses
micropython/pointcloud.py's init_orbital_sampler() (imported via
pc/micropython_shim.py's "run micropython/ modules unmodified under
CPython" trick, the same one pc/orbital_view_pc.py already relies on) and
micropython/cloud_common.py's ORBITAL_PRESETS, which orbital_library.h's own
header comment already requires to stay index-matched to that file's
kOrbitalLibrary -- so this generator and the on-device descriptor table
share one source of truth for preset order, and the sampled math itself is
the SAME MicroPython-port code already cross-validated bit-identical
(double precision) against src/orbitals.h/pointcloud.h's C++ port and the JS
reference by tools/orbitals_host/run_crosscheck.sh, not a third
reimplementation.

Schema (see src/orbital_library.cpp's orbitalInit()/findOrbitalSampler()): a
2-value header (count, tableSize) then `count` fixed-size records, index-
matched 1:1 to ORBITAL_PRESETS/kOrbitalLibrary -- no separate offset/index
table is needed (unlike hfs_table_gen.py's element->subshell-list case):
kOrbitalLibrary itself stays compiled into firmware (36 small descriptor
entries), so a preset's index in that array IS its record's position here.

Flat binary layout (little-endian, packed with `struct`):
    <HH                 count, tableSize
    (record) * count, each:
      <iii              n, ell, m (int32 each -- redundant with
                         kOrbitalLibrary[index], kept as an on-disk
                         self-check/debug read, see src/orbital_library.cpp)
      <f                maxR (float32 -- MUST be float32: this project's
                         ESP32 build always uses orb_real_t=float, see
                         src/orbitals.h/platformio.ini, even though this
                         generator computes in double precision like every
                         other MicroPython-port consumer)
      <1001f            invRTable
      <1001f            invThetaTable
      <1001f            invPhiTable

Output:
    data/orbital_samplers.bin  staged into the PlatformIO SPIFFS image and
                                flashed to the "storage" partition
                                (partitions_16M.csv) via `pio run -t
                                uploadfs` (chained onto every `pio run -t
                                upload`, see tools/extra_script_uploadfs.py).
                                Regenerate and reflash whenever
                                orbital_library.h's kOrbitalLibrary /
                                cloud_common.ORBITAL_PRESETS changes.

Usage:
    python3 tools/orbital_table_gen.py
"""

import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, os.path.join(ROOT, 'pc'))
sys.path.insert(0, os.path.join(ROOT, 'micropython'))

import micropython_shim  # noqa: F401,E402 -- must precede micropython/ imports, see that module

import cloud_common  # noqa: E402
import pointcloud  # noqa: E402

HEADER_FMT = '<HH'   # count, tableSize
FIELDS_FMT = '<iii'  # n, ell, m


def emit_binary(presets):
    """Sample every preset's OrbitalSampler and flatten to (header_bytes, [record_bytes...])
    -- see module docstring for the layout."""
    table_size = None
    records = []
    for n, ell, m, _label in presets:
        sampler = pointcloud.init_orbital_sampler(n, ell, m)
        if table_size is None:
            table_size = len(sampler.inv_r_table)
        assert len(sampler.inv_r_table) == table_size
        assert len(sampler.inv_theta_table) == table_size
        assert len(sampler.inv_phi_table) == table_size

        record = struct.pack(FIELDS_FMT, sampler.n, sampler.ell, sampler.m)
        record += struct.pack('<f', sampler.max_r)
        record += struct.pack('<%df' % table_size, *sampler.inv_r_table)
        record += struct.pack('<%df' % table_size, *sampler.inv_theta_table)
        record += struct.pack('<%df' % table_size, *sampler.inv_phi_table)
        records.append(record)

    header = struct.pack(HEADER_FMT, len(presets), table_size)
    return b''.join([header] + records), table_size


def main():
    blob, table_size = emit_binary(cloud_common.ORBITAL_PRESETS)

    data_bin_path = os.path.join(ROOT, 'data', 'orbital_samplers.bin')
    os.makedirs(os.path.dirname(data_bin_path), exist_ok=True)
    with open(data_bin_path, 'wb') as f:
        f.write(blob)

    print("wrote %s (%d presets, %d pts/table, %d bytes)" %
          (data_bin_path, len(cloud_common.ORBITAL_PRESETS), table_size, len(blob)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
