/**
 * @file storage_mount.h
 * @brief Shared helper to mount the "storage" SPIFFS partition (partitions_16M.csv) at
 *        /storage -- used by every on-demand flash-table reader (physics/hfs_radial.cpp,
 *        physics/orbital_library.cpp), which previously each carried their own byte-for-byte
 *        copy of this logic. NOT used by debug/screenshot.cpp, which mounts the same partition
 *        itself but owns format_if_mount_failed=true for it (screenshots are the one thing
 *        allowed to format a corrupt partition; a stale/missing data file should never trigger
 *        that -- see this header's ensureStorageMounted() docstring).
 */
#pragma once

/**
 * Registers the "storage" SPIFFS partition at /storage if not already mounted. Never formats
 * on a missing/corrupt filesystem (format_if_mount_failed=false) -- a blank partition just
 * means `pio run -t uploadfs` hasn't been run yet, not something to fix by wiping it (which
 * would also risk any device-captured screenshots already on it). ESP_ERR_INVALID_STATE means
 * some other caller (screenshot::init(), most boot paths) already mounted it -- that's success
 * here too, not a retry.
 *
 * Idempotent (safe to call every time, from any boot path) and cheap to call repeatedly once
 * mounted (esp_vfs_spiffs_register() just returns ESP_ERR_INVALID_STATE).
 *
 * @return true if the partition is mounted (by this call or an earlier one), false on a real
 *         mount failure.
 */
bool ensureStorageMounted();
