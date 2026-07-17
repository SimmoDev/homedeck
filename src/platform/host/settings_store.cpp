#include "platform/host/settings_store.h"

#include "platform/host/file_backed_store.h"

namespace homedeck {

HostSettingsStore::HostSettingsStore(std::filesystem::path root_dir) : root_dir_(std::move(root_dir)) {}

std::filesystem::path HostSettingsStore::PathFor(const std::string& ns, const std::string& key) const {
    return root_dir_ / "settings" / ns / key;
}

bool HostSettingsStore::Set(const std::string& ns, const std::string& key, const std::string& value) {
    return WriteFile(PathFor(ns, key), value);
}

std::optional<std::string> HostSettingsStore::Get(const std::string& ns, const std::string& key) {
    return ReadFile(PathFor(ns, key));
}

bool HostSettingsStore::Erase(const std::string& ns, const std::string& key) {
    return EraseFile(PathFor(ns, key));
}

}  // namespace homedeck
