#pragma once

#include "platform/cache_store.h"

#include "wear_levelling.h"

namespace homedeck {

// Mounts the `storage` FAT partition (reserved by
// docs/decisions/ADR-0017-partition-table.md, not previously used) via
// esp_vfs_fat_spiflash_mount_rw_wl(), once at construction, then does
// plain file I/O against it - the internal flash filesystem tier of
// docs/decisions/ADR-0012-storage-tiers.md. `ns` maps onto a
// subdirectory, created on first write.
class FirmwareCacheStore : public CacheStore {
public:
    FirmwareCacheStore();
    ~FirmwareCacheStore() override;

    FirmwareCacheStore(const FirmwareCacheStore&) = delete;
    FirmwareCacheStore& operator=(const FirmwareCacheStore&) = delete;

    bool Write(const std::string& ns, const std::string& key, const std::string& content) override;
    std::optional<std::string> Read(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;

private:
    std::string PathFor(const std::string& ns, const std::string& key) const;

    wl_handle_t wl_handle_ = WL_INVALID_HANDLE;
    bool mounted_ = false;
};

}  // namespace homedeck
