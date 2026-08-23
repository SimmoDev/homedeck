#include "platform/firmware/settings_store.h"

#include "platform/firmware/nvs_blob_store.h"

#include "esp_log.h"
#include "nvs.h"

namespace homedeck {

namespace {
constexpr char kTag[] = "settings_store";
}  // namespace

bool FirmwareSettingsStore::Set(const std::string& ns, const std::string& key, const std::string& value) {
    return NvsSetBlob(NVS_DEFAULT_PART_NAME, kTag, ns, key, value);
}

std::optional<std::string> FirmwareSettingsStore::Get(const std::string& ns, const std::string& key) {
    return NvsGetBlob(NVS_DEFAULT_PART_NAME, kTag, ns, key);
}

std::vector<SettingsEntry> FirmwareSettingsStore::ListAll() {
    // Scoped to the default partition explicitly - SecretStore lives on
    // its own separate partition now (see
    // docs/decisions/ADR-0027-secret-store-partition-separation.md), so
    // this structurally never sees a secret, not just via the
    // module_id/key guard Storage's own reserved-key check still applies
    // on top. nvs_entry_info only yields (namespace, key, type), not the
    // value itself - the actual read reuses Get() below rather than
    // duplicating nvs_get_blob's size-probe-then-read dance here.
    std::vector<std::pair<std::string, std::string>> ns_keys;
    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        esp_err_t info_err = nvs_entry_info(it, &info);
        // Reached only right after nvs_entry_find()/nvs_entry_next() both
        // already returned ESP_OK for this same iterator, so this is
        // effectively unreachable with the current NVS driver - checked
        // regardless, same as every other esp_*() call in this codebase.
        // Skipping the entry (rather than aborting the whole listing) on
        // an unexpected failure keeps one bad entry from hiding every
        // other setting from a backup export.
        if (info_err != ESP_OK) {
            ESP_LOGW(kTag, "nvs_entry_info() failed: %s - skipping this entry", esp_err_to_name(info_err));
        } else {
            ns_keys.emplace_back(info.namespace_name, info.key);
        }
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
    return NvsEraseBlob(NVS_DEFAULT_PART_NAME, ns, key);
}

}  // namespace homedeck
