#include "screenshot.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "png_writer.h"

namespace screenshot
{
namespace
{
    constexpr auto kTag = "screenshot";
    constexpr auto kMountPoint = "/storage";
    // Must match the "storage" partition name in partitions_16M.csv.
    constexpr auto kPartitionLabel = "storage";
    // "/storage/" (9) + the VFS dirent's max d_name width (255) + nul -- sized so GCC's
    // -Wformat-truncation can statically prove the snprintf()s below never truncate (it
    // checks against d_name's declared array bound, not SPIFFS's real 32-char name limit).
    constexpr size_t kMaxPathLen = 300;

    bool nextFilename(char *outName, size_t cap)
    {
        int maxIndex = 0;
        DIR *dir = opendir(kMountPoint);
        if (dir != nullptr)
        {
            struct dirent *entry;
            while ((entry = readdir(dir)) != nullptr)
            {
                int index = 0;
                if (std::sscanf(entry->d_name, "shot_%d.png", &index) == 1 && index > maxIndex)
                    maxIndex = index;
            }
            closedir(dir);
        }
        return std::snprintf(outName, cap, "shot_%04d.png", maxIndex + 1) < int(cap);
    }

    /// PNG-encodes `frameBuf` and writes it to `path` (a full "/storage/..." path). Shared by
    /// capture() (auto-numbered name) and captureAs() (caller-chosen name).
    bool encodeAndWriteTo(const uint16_t *frameBuf, const char *path, size_t *outSize)
    {
        size_t need = png_writer::requiredBufferSize(Display::kDisplayWidth, Display::kDisplayHeight);
        uint8_t *buf = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        if (buf == nullptr)
        {
            ESP_LOGE(kTag, "failed to allocate %u-byte PNG buffer", (unsigned)need);
            return false;
        }
        size_t written = png_writer::encodeRgb565(frameBuf, Display::kDisplayWidth, Display::kDisplayHeight, buf, need);
        if (written == 0)
        {
            ESP_LOGE(kTag, "PNG encode failed");
            heap_caps_free(buf);
            return false;
        }
        FILE *f = fopen(path, "wb");
        if (f == nullptr)
        {
            ESP_LOGE(kTag, "failed to open %s for writing", path);
            heap_caps_free(buf);
            return false;
        }
        size_t wrote = fwrite(buf, 1, written, f);
        fclose(f);
        heap_caps_free(buf);
        if (wrote != written)
        {
            ESP_LOGE(kTag, "short write to %s (%u/%u bytes)", path, (unsigned)wrote, (unsigned)written);
            return false;
        }
        if (outSize != nullptr)
            *outSize = written;
        return true;
    }
} // namespace

void init()
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kMountPoint;
    conf.partition_label = kPartitionLabel;
    conf.max_files = 8;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK)
    {
        ESP_LOGE(kTag, "storage mount failed: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(kPartitionLabel, &total, &used);
    ESP_LOGI(kTag, "storage mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
}

bool capture(const uint16_t *frameBuf, char *outName, size_t outNameCapacity, size_t *outSize)
{
    if (frameBuf == nullptr || outName == nullptr)
        return false;

    char name[kMaxNameLen];
    if (!nextFilename(name, sizeof(name)))
        return false;

    char path[kMaxPathLen];
    std::snprintf(path, sizeof(path), "%s/%s", kMountPoint, name);
    size_t written = 0;
    if (!encodeAndWriteTo(frameBuf, path, &written))
        return false;

    std::snprintf(outName, outNameCapacity, "%s", name);
    if (outSize != nullptr)
        *outSize = written;
    ESP_LOGI(kTag, "captured %s (%u bytes)", name, (unsigned)written);
    return true;
}

bool captureAs(const uint16_t *frameBuf, const char *name, size_t *outSize)
{
    if (frameBuf == nullptr || name == nullptr)
        return false;

    char path[kMaxPathLen];
    std::snprintf(path, sizeof(path), "%s/%s", kMountPoint, name);
    size_t written = 0;
    if (!encodeAndWriteTo(frameBuf, path, &written))
        return false;

    if (outSize != nullptr)
        *outSize = written;
    ESP_LOGI(kTag, "captured %s (%u bytes)", name, (unsigned)written);
    return true;
}

void forEachFile(FileVisitor visit, void *ctx)
{
    DIR *dir = opendir(kMountPoint);
    if (dir == nullptr)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        char path[kMaxPathLen];
        std::snprintf(path, sizeof(path), "%s/%s", kMountPoint, entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0)
            visit(entry->d_name, size_t(st.st_size), ctx);
    }
    closedir(dir);
}

uint8_t *readFile(const char *name, size_t *outSize)
{
    char path[kMaxPathLen];
    std::snprintf(path, sizeof(path), "%s/%s", kMountPoint, name);
    FILE *f = fopen(path, "rb");
    if (f == nullptr)
        return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0)
    {
        fclose(f);
        return nullptr;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(size_t(size), MALLOC_CAP_SPIRAM);
    if (buf == nullptr)
    {
        fclose(f);
        return nullptr;
    }
    size_t read = fread(buf, 1, size_t(size), f);
    fclose(f);
    if (read != size_t(size))
    {
        heap_caps_free(buf);
        return nullptr;
    }
    if (outSize != nullptr)
        *outSize = size_t(size);
    return buf;
}

bool deleteFile(const char *name)
{
    char path[kMaxPathLen];
    std::snprintf(path, sizeof(path), "%s/%s", kMountPoint, name);
    return remove(path) == 0;
}

} // namespace screenshot
