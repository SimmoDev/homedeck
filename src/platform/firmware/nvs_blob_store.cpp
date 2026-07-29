#include "platform/firmware/nvs_blob_store.h"

#include "platform/store_key_validation.h"

#include "esp_log.h"
#include "nvs.h"

namespace homedeck {

bool NvsSetBlob(const char* partition_name, const char* tag, const std::string& ns, const std::string& key,
                 const std::string& value) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(partition_name, ns.c_str(), NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "nvs_open_from_partition(%s, %s) failed: %s", partition_name, ns.c_str(),
                 esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, key.c_str(), value.data(), value.size());
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "nvs_set_blob(%s/%s) failed: %s", ns.c_str(), key.c_str(), esp_err_to_name(err));
        return false;
    }
    return true;
}

std::optional<std::string> NvsGetBlob(const char* partition_name, const char* tag, const std::string& ns,
                                       const std::string& key) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return std::nullopt;
    }
    nvs_handle_t handle;
    // A namespace that's never been written to yet fails to open in
    // read-only mode - a normal "no value" case, not an error worth
    // logging.
    esp_err_t err = nvs_open_from_partition(partition_name, ns.c_str(), NVS_READONLY, &handle);
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
        ESP_LOGE(tag, "nvs_get_blob(%s/%s) failed: %s", ns.c_str(), key.c_str(), esp_err_to_name(err));
        return std::nullopt;
    }
    return value;
}

bool NvsEraseBlob(const char* partition_name, const std::string& ns, const std::string& key) {
    if (!IsValidStoreSegment(ns) || !IsValidStoreSegment(key)) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(partition_name, ns.c_str(), NVS_READWRITE, &handle);
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
