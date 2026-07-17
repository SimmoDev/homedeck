#pragma once

#include "platform/settings_store.h"

namespace homedeck {

// Wraps ESP-IDF's NVS API directly against the `nvs` partition - see
// docs/decisions/ADR-0017-partition-table.md. Plain (unencrypted) storage
// for now - see docs/decisions/ADR-0010-secret-storage.md and the M2
// Configuration-service roadmap item for why activating the HMAC-secured
// scheme is a separate, deliberately deferred step, not done here.
// `ns`/`key` map directly onto NVS's own namespace/key strings, which are
// capped at 15 characters (NVS_KEY_NAME_MAX_SIZE - 1) - callers exceeding
// that get an ESP_ERR_NVS_INVALID_NAME failure surfaced as a normal
// false/nullopt return, not a crash.
//
// Requires nvs_flash_init() to have already been called (already done in
// firmware/main/homedeck.cpp's app_main(), ahead of Wi-Fi bring-up).
class FirmwareSettingsStore : public SettingsStore {
public:
    FirmwareSettingsStore() = default;

    bool Set(const std::string& ns, const std::string& key, const std::string& value) override;
    std::optional<std::string> Get(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;
};

}  // namespace homedeck
