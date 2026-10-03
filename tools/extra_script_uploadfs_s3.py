# extra_script_uploadfs_s3.py
#
# PlatformIO's built-in `uploadfs` target shells out to `mkspiffs` (tool-mkspiffs's
# mkspiffs_espressif32_espidf binary), which is armhf-only -- it won't run on an aarch64 host
# (e.g. a Jetson) without bootstrapping a full armhf multiarch userland (dpkg --add-architecture
# armhf + libc6:armhf), which qemu-user-static alone does not provide. See CYD-branch.md for
# the hardware-verified story.
#
# This defines an equivalent custom target, `uploadfs_s3`, that builds the same data/
# directory into a SPIFFS image via ESP-IDF's own pure-Python spiffsgen.py (no native tool
# involved at all) and writes it straight to the "storage" partition's offset/size, read from
# board_build.partitions (partitions_16M.csv) so this stays correct if that table changes.
# Same approach verified on the CYD env (`uploadfs_cyd`); `pio run -e WS_ESP32_S3_LCD_1_3 -t uploadfs_s3` then a power
# cycle mounted the partition with the expected byte count and loaded the orbital sampler
# table successfully.
#
# Usage: pio run -e WS_ESP32_S3_LCD_1_3 -t uploadfs_s3
import csv
import os

Import("env")


def _parse_int(text):
    # partition CSVs allow K/M suffixes (e.g. "7M")
    mult = {"K": 1024, "M": 1024 * 1024}.get(text[-1:].upper(), 1)
    return int(text[:-1] if mult != 1 else text, 0) * mult


def _storage_partition_offset_and_size(csv_path):
    with open(csv_path, encoding="utf-8") as f:
        for row in csv.reader(f):
            row = [c.strip() for c in row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            name, _type, _subtype, offset, size = (row + [""] * 5)[:5]
            if name == "storage":
                return _parse_int(offset), _parse_int(size)
    raise RuntimeError(f"no 'storage' partition found in {csv_path}")


def _idf_path(env):
    idf_path = env["ENV"].get("IDF_PATH")
    if idf_path and os.path.isdir(idf_path):
        return idf_path
    # Fallback: PlatformIO's espidf framework package, same layout as IDF_PATH.
    candidate = env.PioPlatform().get_package_dir("framework-espidf")
    if candidate and os.path.isdir(candidate):
        return candidate
    raise RuntimeError("could not locate IDF_PATH (needed for components/spiffs/spiffsgen.py)")


def uploadfs_s3(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    partitions_csv = os.path.join(project_dir, env.GetProjectOption("board_build.partitions"))
    offset, size = _storage_partition_offset_and_size(partitions_csv)

    spiffsgen = os.path.join(_idf_path(env), "components", "spiffs", "spiffsgen.py")
    data_dir = os.path.join(project_dir, "data")
    image_path = os.path.join(build_dir, "spiffs_s3.bin")

    print(f"Building SPIFFS image ({size} bytes) from '{data_dir}' via spiffsgen.py -> {image_path}")
    if env.Execute(f'"$PYTHONEXE" "{spiffsgen}" {size} "{data_dir}" "{image_path}"'):
        env.Exit(1)

    esptool_py = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    chip = env.BoardConfig().get("build.mcu", "esp32s3")
    upload_port = env.subst("$UPLOAD_PORT")
    port_args = f"--port {upload_port}" if upload_port and not upload_port.startswith("$") else ""
    print(f"Writing SPIFFS image to storage partition at {hex(offset)}")
    if env.Execute(f'"$PYTHONEXE" "{esptool_py}" --chip {chip} {port_args} write_flash {hex(offset)} "{image_path}"'):
        env.Exit(1)


env.AddCustomTarget(
    name="uploadfs_s3",
    dependencies=None,
    actions=[uploadfs_s3],
    title="Upload Filesystem Image (S3, spiffsgen.py)",
    description=(
        "Build data/ into a SPIFFS image via ESP-IDF's spiffsgen.py and flash it to the "
        "storage partition directly via esptool -- bypasses PlatformIO's mkspiffs (armhf-only, "
        "doesn't run on aarch64 hosts without a full multiarch bootstrap). Same approach as the CYD env."
    ),
)
