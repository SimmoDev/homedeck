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

std::vector<SettingsEntry> HostSettingsStore::ListAll() {
    std::vector<SettingsEntry> entries;
    std::filesystem::path settings_dir = root_dir_ / "settings";
    if (!std::filesystem::exists(settings_dir)) return entries;
    for (const auto& ns_entry : std::filesystem::directory_iterator(settings_dir)) {
        if (!ns_entry.is_directory()) continue;
        std::string ns = ns_entry.path().filename().string();
        for (const auto& key_entry : std::filesystem::directory_iterator(ns_entry.path())) {
            if (!key_entry.is_regular_file()) continue;
            std::optional<std::string> value = ReadFile(key_entry.path());
            if (!value.has_value()) continue;
            entries.push_back({ns, key_entry.path().filename().string(), *value});
        }
    }
    return entries;
}

}  // namespace homedeck
