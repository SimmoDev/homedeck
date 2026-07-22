#include "platform/firmware/settings_store.h"

#include "platform/firmware/nvs_blob_store.h"

#include "nvs.h"

namespace homedeck {

namespace {
constexpr char kTag[] = "settings_store";
}  // namespace

bool FirmwareSettingsStore::Set(const std::string& ns, const std::string& key, const std::string& value) {
    return NvsSetBlob(kTag, ns, key, value);
}

std::optional<std::string> FirmwareSettingsStore::Get(const std::string& ns, const std::string& key) {
    return NvsGetBlob(kTag, ns, key);
}

std::vector<SettingsEntry> FirmwareSettingsStore::ListAll() {
    // nvs_entry_info only yields (namespace, key, type), not the value
    // itself - the actual read reuses Get() below rather than
    // duplicating nvs_get_blob's size-probe-then-read dance here.
    std::vector<std::pair<std::string, std::string>> ns_keys;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        ns_keys.emplace_back(info.namespace_name, info.key);
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);

    std::vector<SettingsEntry> entries;
    for (auto& [ns, key] : ns_keys) {
        std::optional<std::string> value = Get(ns, key);
        if (value.has_value()) entries.push_back({ns, key, *value});
    }
    return entries;
}

bool FirmwareSettingsStore::Erase(const std::string& ns, const std::string& key) {
    return NvsEraseBlob(ns, key);
}

}  // namespace homedeck
