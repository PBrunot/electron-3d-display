"""pc/pull_screenshots.py -- recovers on-device screenshots (see src/debug/screenshot.h /
src/debug/screenshot_console.h) over the same serial console `pio device monitor` already uses for
logs and flashing. No new USB peripheral or filesystem exposure on the device side: this just
drives the SS_LIST/SS_GET/SS_DEL line protocol from screenshot_console.cpp over UART0, and
ignores any interleaved ESP_LOG output (anything not starting with "SS_").

On-device screenshots are already PNG-compressed (png_writer.cpp, LZ77 + DEFLATE's fixed
Huffman table) -- this script just moves the bytes, it doesn't re-encode anything.

SS_LIST enumerates the WHOLE "storage" SPIFFS partition (screenshot::forEachFile() walks its
mount root directly), which also holds data/hfs_tables.bin, data/orbital_samplers.bin, and
data/atomic_cube.jpg (see partitions_16M.csv) -- not just screenshots. --list/--all filter to
*.png only, so this stays a screenshot puller instead of also listing/pulling those unrelated
multi-hundred-KB data blobs over the same slow base64-over-serial link.

Usage:
    python3 pc/pull_screenshots.py --list
    python3 pc/pull_screenshots.py --all                       # pull every device screenshot
    python3 pc/pull_screenshots.py shot_0001.png shot_0003.png  # pull specific files
    python3 pc/pull_screenshots.py --all --delete               # ...then delete them on-device
                                                                  # (only after a verified pull)

Requires pyserial (`pip install pyserial`). Port defaults to /dev/ttyACM0 (this project's WSL
setup, see CLAUDE.md) -- override with --port. Close any open `pio device monitor` first: only
one process can hold the serial port at a time.
"""

import argparse
import base64
import binascii
import os
import sys
import time

import serial

DEFAULT_PORT = '/dev/ttyACM0'
DEFAULT_BAUD = 115200
OUT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'screenshots'))
LINE_TIMEOUT_S = 5.0


def _read_ss_line(ser, deadline):
    """Read lines until one starting with 'SS_' arrives (dropping interleaved ESP_LOG output
    along the way) or the deadline passes."""
    while time.monotonic() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode('utf-8', errors='replace').strip()
        if line.startswith('SS_'):
            return line
        if line:
            print('  (log) %s' % line)
    return None


def list_files(ser):
    """Every file SS_LIST reports -- the whole "storage" partition, not just screenshots (see
    module docstring). Callers that want screenshots only should filter with is_screenshot()."""
    ser.write(b'SS_LIST\n')
    deadline = time.monotonic() + LINE_TIMEOUT_S
    files = []
    while True:
        line = _read_ss_line(ser, deadline)
        if line is None:
            print('timed out waiting for SS_LIST_END')
            break
        if line == 'SS_LIST_END':
            break
        parts = line.split()
        if len(parts) == 3 and parts[0] == 'SS_FILE':
            files.append((parts[1], int(parts[2])))
    return files


def is_screenshot(name):
    return name.lower().endswith('.png')


def pull_file(ser, name, out_dir):
    ser.write(('SS_GET %s\n' % name).encode('ascii'))
    deadline = time.monotonic() + LINE_TIMEOUT_S
    begin = _read_ss_line(ser, deadline)
    if begin is None or not begin.startswith('SS_BEGIN '):
        print('  %s: no SS_BEGIN (got %r)' % (name, begin))
        return False
    _, got_name, size_str, crc_str = begin.split()
    expected_size = int(size_str)
    expected_crc = int(crc_str, 16)

    chunks = []
    while True:
        line = _read_ss_line(ser, deadline)
        if line is None:
            print('  %s: timed out mid-transfer' % name)
            return False
        if line == 'SS_END':
            break
        if line.startswith('SS_DATA '):
            chunks.append(line[len('SS_DATA '):])
        deadline = time.monotonic() + LINE_TIMEOUT_S  # reset on every line actually received

    data = base64.b64decode(''.join(chunks))
    if len(data) != expected_size:
        print('  %s: size mismatch (got %d, expected %d)' % (name, len(data), expected_size))
        return False
    actual_crc = binascii.crc32(data) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        print('  %s: CRC mismatch (got %08x, expected %08x) -- likely a log line interleaved '
              'mid-transfer, retry' % (name, actual_crc, expected_crc))
        return False

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, got_name)
    with open(path, 'wb') as f:
        f.write(data)
    print('  saved %s (%d bytes, verified)' % (path, len(data)))
    return True


def delete_file(ser, name):
    ser.write(('SS_DEL %s\n' % name).encode('ascii'))
    deadline = time.monotonic() + LINE_TIMEOUT_S
    line = _read_ss_line(ser, deadline)
    return line is not None and line.startswith('SS_DELETED')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('names', nargs='*', help='specific screenshot filenames to pull')
    parser.add_argument('--list', action='store_true', help='list on-device screenshots and exit')
    parser.add_argument('--all', action='store_true', help='pull every on-device screenshot')
    parser.add_argument('--delete', action='store_true',
                        help='delete each file on-device after a verified pull')
    parser.add_argument('--port', default=DEFAULT_PORT)
    parser.add_argument('--baud', type=int, default=DEFAULT_BAUD)
    parser.add_argument('--out-dir', default=OUT_DIR)
    args = parser.parse_args()

    if not args.list and not args.all and not args.names:
        parser.error('specify --list, --all, or one or more filenames')

    ser = serial.Serial(args.port, args.baud, timeout=0.5)
    time.sleep(0.2)  # let the port settle before the first command
    try:
        files = [(n, s) for n, s in list_files(ser) if is_screenshot(n)]
        print('on-device: %d screenshot(s)' % len(files))
        for name, size in files:
            print('  %s (%d bytes)' % (name, size))

        if args.list:
            return

        targets = [n for n, _ in files] if args.all else args.names
        ok = 0
        for name in targets:
            if pull_file(ser, name, args.out_dir):
                ok += 1
                if args.delete:
                    delete_file(ser, name)
        print('%d/%d pulled successfully' % (ok, len(targets)))
        if ok != len(targets):
            sys.exit(1)
    finally:
        ser.close()


if __name__ == '__main__':
    main()
