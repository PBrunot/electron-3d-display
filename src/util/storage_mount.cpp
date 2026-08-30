#include "util/storage_mount.h"

#include "esp_spiffs.h"

namespace
{
    constexpr auto kMountPoint = "/storage";
    constexpr auto kPartitionLabel = "storage";
} // namespace

bool ensureStorageMounted()
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kMountPoint;
    conf.partition_label = kPartitionLabel;
    // At most one flash-table file is held open at a time (hfs_radial.cpp/orbital_library.cpp
    // each keep a single sDataFile for the process lifetime): a small max_files keeps the SPIFFS
    // cache buffer small enough to fit in whatever's left of the DMA-capable heap after the
    // display's frame buffer allocation (see display.cpp's block-splitting comment).
    conf.max_files = 2;
    conf.format_if_mount_failed = false;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}
