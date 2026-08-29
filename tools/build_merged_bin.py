#!/usr/bin/env python3
# build_merged_bin.py
#
# Builds the app + SPIFFS image for one environment, then merges bootloader +
# partition table + app + SPIFFS into a single .bin via esptool's merge_bin,
# ready to flash with one command:
#   esptool.py --chip <chip> write_flash 0x0 <merged bin>
# (or the equivalent from a flashing tool like ESP Flash Download Tool /
# esptool-js -- a merged image is the format those expect for "one file" flashing.)
#
# Partition offsets are read straight from each env's partitions_*.csv (same
# approach as tools/extra_script_uploadfs_cyd.py's storage-offset lookup) so
# this stays correct if that table changes. flash_size is computed from the
# highest partition end in that CSV rather than trusted from PlatformIO's
# generated flasher_args.json -- for CYD that file currently reports 16MB
# (sdkconfig.defaults's S3-oriented 16MB setting leaks into every env because
# PlatformIO always applies the project-root sdkconfig.defaults in addition to
# board_build.esp-idf.sdkconfig_defaults, contradicting sdkconfig.defaults.CYD's
# own header comment) even though partitions_cyd.csv's own header says this
# board has 4MB flash and the table ends exactly at 0x400000.
#
# Usage: python3 tools/build_merged_bin.py <CYD|WS_ESP32_S3_LCD_1_3>
import argparse
import csv
import json
import os
import shutil
import subprocess
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ENV_PARTITIONS_CSV = {
    "CYD": "partitions_cyd.csv",
    "WS_ESP32_S3_LCD_1_3": "partitions_16M.csv",
}

FLASH_SIZE_CHOICES_MB = [1, 2, 4, 8, 16, 32]


def parse_size(size_s):
    size_s = size_s.strip()
    if size_s[-1:] in ("K", "M"):
        mult = 1024 if size_s[-1] == "K" else 1024 * 1024
        return int(size_s[:-1], 0) * mult
    return int(size_s, 0)


def read_partitions(csv_path):
    rows = []
    with open(csv_path, encoding="utf-8") as f:
        for row in csv.reader(f):
            row = [c.strip() for c in row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            name, ptype, subtype, offset_s, size_s = (row + [""] * 5)[:5]
            rows.append((name, ptype, subtype, int(offset_s, 0), parse_size(size_s)))
    return rows


def find_app_and_storage(rows, csv_path):
    app = next((r for r in rows if r[1] == "app"), None)
    storage = next((r for r in rows if r[0] == "storage"), None)
    if app is None:
        raise SystemExit(f"no 'app' type partition found in {csv_path}")
    if storage is None:
        raise SystemExit(f"no 'storage' partition found in {csv_path}")
    return app, storage


def flash_size_label(rows):
    max_end = max(offset + size for _, _, _, offset, size in rows)
    for mb in FLASH_SIZE_CHOICES_MB:
        if max_end <= mb * 1024 * 1024:
            return f"{mb}MB"
    raise SystemExit(f"partition table extends to {max_end} bytes, beyond largest known flash size choice")


def find_esptool():
    exe = shutil.which("esptool.py") or shutil.which("esptool")
    if exe:
        return [exe]
    packaged = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")
    if os.path.isfile(packaged):
        return [sys.executable, packaged]
    raise SystemExit("esptool not found (pip install esptool, or install PlatformIO's tool-esptoolpy package)")


def run(cmd, **kwargs):
    print("+ " + " ".join(cmd))
    subprocess.run(cmd, cwd=PROJECT_DIR, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("env", choices=sorted(ENV_PARTITIONS_CSV))
    parser.add_argument("-o", "--output", help="output path (default: .pio/build/<env>/merged-flash.bin)")
    args = parser.parse_args()

    csv_path = os.path.join(PROJECT_DIR, ENV_PARTITIONS_CSV[args.env])
    rows = read_partitions(csv_path)
    (app_name, _, app_subtype, app_offset, _), (_, _, _, storage_offset, _) = find_app_and_storage(rows, csv_path)
    flash_size = flash_size_label(rows)

    pio = shutil.which("pio") or shutil.which("platformio")
    if not pio:
        raise SystemExit("'pio' not found on PATH (PlatformIO CLI)")

    build_dir = os.path.join(PROJECT_DIR, ".pio", "build", args.env)
    run([pio, "run", "-e", args.env])
    run([pio, "run", "-e", args.env, "-t", "buildfs"])

    flasher_args_path = os.path.join(build_dir, "flasher_args.json")
    with open(flasher_args_path, encoding="utf-8") as f:
        flasher_args = json.load(f)
    bootloader_offset = flasher_args["bootloader"]["offset"]
    chip = flasher_args["extra_esptool_args"]["chip"]
    flash_mode = flasher_args["flash_settings"]["flash_mode"]
    flash_freq = flasher_args["flash_settings"]["flash_freq"]

    output = args.output or os.path.join(build_dir, "merged-flash.bin")

    print(f"App partition: {app_name} ({app_subtype}) at {hex(app_offset)}; storage at {hex(storage_offset)}; "
          f"flash_size={flash_size} (computed from {os.path.basename(csv_path)}, chip={chip})")

    cmd = find_esptool() + [
        "--chip", chip,
        "merge_bin",
        "-o", output,
        "--flash_mode", flash_mode,
        "--flash_freq", flash_freq,
        "--flash_size", flash_size,
        bootloader_offset, os.path.join(build_dir, "bootloader.bin"),
        "0x8000", os.path.join(build_dir, "partitions.bin"),
        hex(app_offset), os.path.join(build_dir, "firmware.bin"),
        hex(storage_offset), os.path.join(build_dir, "spiffs.bin"),
    ]
    run(cmd)

    print(f"\nMerged image: {output}")
    print(f"Flash with:  esptool.py --chip {chip} write_flash 0x0 {os.path.relpath(output, PROJECT_DIR)}")


if __name__ == "__main__":
    main()
