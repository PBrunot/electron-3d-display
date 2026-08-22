# extra_script_uploadfs_cyd.py
#
# PlatformIO's built-in `uploadfs` target shells out to `mkspiffs` (tool-mkspiffs's
# mkspiffs_espressif32_espidf binary), which is armhf-only -- it won't run on an aarch64 host
# (e.g. a Jetson) without bootstrapping a full armhf multiarch userland (dpkg --add-architecture
# armhf + libc6:armhf), which qemu-user-static alone does not provide. See CYD-branch.md for
# the hardware-verified story.
#
# This defines an equivalent custom target, `uploadfs_cyd`, that builds the same data/
# directory into a SPIFFS image via ESP-IDF's own pure-Python spiffsgen.py (no native tool
# involved at all) and writes it straight to the "storage" partition's offset/size, read from
# board_build.partitions (partitions_cyd.csv) so this stays correct if that table changes.
# Verified end to end on real CYD hardware: `pio run -e CYD -t uploadfs_cyd` then a power
# cycle mounted the partition with the expected byte count and loaded the orbital sampler
# table successfully.
#
# Usage: pio run -e CYD -t uploadfs_cyd
import csv
import os

Import("env")


def _storage_partition_offset_and_size(csv_path):
    with open(csv_path, encoding="utf-8") as f:
        for row in csv.reader(f):
            row = [c.strip() for c in row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            name, _type, _subtype, offset, size = (row + [""] * 5)[:5]
            if name == "storage":
                return int(offset, 0), int(size, 0)
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


def uploadfs_cyd(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    partitions_csv = os.path.join(project_dir, env.GetProjectOption("board_build.partitions"))
    offset, size = _storage_partition_offset_and_size(partitions_csv)

    spiffsgen = os.path.join(_idf_path(env), "components", "spiffs", "spiffsgen.py")
    data_dir = os.path.join(project_dir, "data")
    image_path = os.path.join(build_dir, "spiffs_cyd.bin")

    print(f"Building SPIFFS image ({size} bytes) from '{data_dir}' via spiffsgen.py -> {image_path}")
    if env.Execute(f'"$PYTHONEXE" "{spiffsgen}" {size} "{data_dir}" "{image_path}"'):
        env.Exit(1)

    esptool_py = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    upload_port = env.subst("$UPLOAD_PORT")
    port_args = f"--port {upload_port}" if upload_port and not upload_port.startswith("$") else ""
    print(f"Writing SPIFFS image to storage partition at {hex(offset)}")
    if env.Execute(f'"$PYTHONEXE" "{esptool_py}" --chip esp32 {port_args} write_flash {hex(offset)} "{image_path}"'):
        env.Exit(1)


env.AddCustomTarget(
    name="uploadfs_cyd",
    dependencies=None,
    actions=[uploadfs_cyd],
    title="Upload Filesystem Image (CYD, spiffsgen.py)",
    description=(
        "Build data/ into a SPIFFS image via ESP-IDF's spiffsgen.py and flash it to the "
        "storage partition directly via esptool -- bypasses PlatformIO's mkspiffs (armhf-only, "
        "doesn't run on aarch64 hosts without a full multiarch bootstrap). See CYD-branch.md."
    ),
)
