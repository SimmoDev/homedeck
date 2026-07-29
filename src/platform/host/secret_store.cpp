#include "platform/host/secret_store.h"

#include "platform/host/file_backed_store.h"
#include "platform/store_key_validation.h"

namespace homedeck {

HostSecretStore::HostSecretStore(std::filesystem::path root_dir) : root_dir_(std::move(root_dir)) {}

std::filesystem::path HostSecretStore::PathFor(const std::string& ns, const std::string& key) const {
    return root_dir_ / "secrets" / ns / key;
}

bool HostSecretStore::Set(const std::string& ns, const std::string& key, const std::string& value) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    return WriteFile(PathFor(ns, key), value);
}

std::optional<std::string> HostSecretStore::Get(const std::string& ns, const std::string& key) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return std::nullopt;
    }
    return ReadFile(PathFor(ns, key));
}

bool HostSecretStore::Erase(const std::string& ns, const std::string& key) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    return EraseFile(PathFor(ns, key));
}

}  // namespace homedeck
