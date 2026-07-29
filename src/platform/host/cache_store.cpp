#include "platform/host/cache_store.h"

#include "platform/host/file_backed_store.h"
#include "platform/store_key_validation.h"

namespace homedeck {

HostCacheStore::HostCacheStore(std::filesystem::path root_dir) : root_dir_(std::move(root_dir)) {}

std::filesystem::path HostCacheStore::PathFor(const std::string& ns, const std::string& key) const {
    return root_dir_ / "cache" / ns / key;
}

bool HostCacheStore::Write(const std::string& ns, const std::string& key, const std::string& content) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    return WriteFile(PathFor(ns, key), content);
}

std::optional<std::string> HostCacheStore::Read(const std::string& ns, const std::string& key) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return std::nullopt;
    }
    return ReadFile(PathFor(ns, key));
}

bool HostCacheStore::Erase(const std::string& ns, const std::string& key) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    return EraseFile(PathFor(ns, key));
}

}  // namespace homedeck
