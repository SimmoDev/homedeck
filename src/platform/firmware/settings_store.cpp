#include "platform/firmware/settings_store.h"

#include "esp_log.h"
#include "nvs.h"

namespace homedeck {

namespace {
constexpr char kTag[] = "settings_store";
}  // namespace

bool FirmwareSettingsStore::Set(const std::string& ns, const std::string& key, const std::string& value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s) failed: %s", ns.c_str(), esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, key.c_str(), value.data(), value.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_set_blob(%s/%s) failed: %s", ns.c_str(), key.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

std::optional<std::string> FirmwareSettingsStore::Get(const std::string& ns, const std::string& key) {
    nvs_handle_t handle;
    // A namespace that's never been written to yet fails to open in
    // read-only mode - a normal "no value" case, not an error worth
    // logging.
    esp_err_t err = nvs_open(ns.c_str(), NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return std::nullopt;
    }

    size_t size = 0;
    err = nvs_get_blob(handle, key.c_str(), nullptr, &size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return std::nullopt;
    }

    std::string value(size, '\0');
    err = nvs_get_blob(handle, key.c_str(), value.data(), &size);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_get_blob(%s/%s) failed: %s", ns.c_str(), key.c_str(), esp_err_to_name(err));
        return std::nullopt;
    }
    return value;
}

bool FirmwareSettingsStore::Erase(const std::string& ns, const std::string& key) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ns.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return true;
    }

    err = nvs_erase_key(handle, key.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return true;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

}  // namespace homedeck
