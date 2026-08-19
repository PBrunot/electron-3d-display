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
    conf.max_files = 8;
    conf.format_if_mount_failed = false;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}
