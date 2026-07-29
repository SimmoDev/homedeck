#include "platform/firmware/cache_store.h"

#include "platform/store_key_validation.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include <sys/stat.h>

#include <cerrno>
#include <cstdio>

namespace homedeck {

namespace {

constexpr char kTag[] = "cache_store";
constexpr char kMountPoint[] = "/storage";
constexpr char kPartitionLabel[] = "storage";

}  // namespace

FirmwareCacheStore::FirmwareCacheStore() {
    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(kMountPoint, kPartitionLabel, &config, &wl_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to mount '%s' partition: %s", kPartitionLabel, esp_err_to_name(err));
        return;
    }
    mounted_ = true;
}

FirmwareCacheStore::~FirmwareCacheStore() {
    if (mounted_) {
        esp_vfs_fat_spiflash_unmount_rw_wl(kMountPoint, wl_handle_);
    }
}

std::string FirmwareCacheStore::PathFor(const std::string& ns, const std::string& key) const {
    return std::string(kMountPoint) + "/" + ns + "/" + key;
}

bool FirmwareCacheStore::Write(const std::string& ns, const std::string& key, const std::string& content) {
    if (!mounted_ || !IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    std::string dir = std::string(kMountPoint) + "/" + ns;
    // EEXIST is the expected/common case once a module has written
    // anything before - not an error.
    mkdir(dir.c_str(), 0777);

    FILE* file = fopen(PathFor(ns, key).c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGE(kTag, "Failed to open '%s/%s' for writing", ns.c_str(), key.c_str());
        return false;
    }
    size_t written = fwrite(content.data(), 1, content.size(), file);
    fclose(file);
    if (written != content.size()) {
        ESP_LOGE(kTag, "Short write to '%s/%s': %zu of %zu bytes", ns.c_str(), key.c_str(), written, content.size());
        return false;
    }
    return true;
}

std::optional<std::string> FirmwareCacheStore::Read(const std::string& ns, const std::string& key) {
    if (!mounted_ || !IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return std::nullopt;
    }
    FILE* file = fopen(PathFor(ns, key).c_str(), "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0) {
        fclose(file);
        return std::nullopt;
    }
    std::string content(static_cast<size_t>(size), '\0');
    size_t read_bytes = fread(content.data(), 1, content.size(), file);
    fclose(file);
    if (read_bytes != content.size()) {
        return std::nullopt;
    }
    return content;
}

bool FirmwareCacheStore::Erase(const std::string& ns, const std::string& key) {
    if (!mounted_ || !IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    // A file that's already gone is a successful erase, not a failure.
    return std::remove(PathFor(ns, key).c_str()) == 0 || errno == ENOENT;
}

}  // namespace homedeck
