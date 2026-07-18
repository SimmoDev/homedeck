#pragma once

#include "platform/secret_store.h"

namespace homedeck {

// Wraps ESP-IDF's NVS API directly against the `nvs` partition - see
// docs/decisions/ADR-0017-partition-table.md. Plain (unencrypted)
// storage by design at this project stage - see
// docs/decisions/ADR-0018-staged-security-hardening.md for the staged
// security model that decides when NVS encryption activates.
//
// Shares the same physical NVS partition and namespace-per-`ns` as
// FirmwareSettingsStore (unlike HostSecretStore, which uses a separate
// directory) - a caller must not reuse the same (ns, key) pair through
// both stores for the same module, since nothing here prevents that
// collision at the NVS layer. `ns`/`key` are capped at 15 characters
// (NVS_KEY_NAME_MAX_SIZE - 1), same as FirmwareSettingsStore.
//
// Requires nvs_flash_init() to have already been called (already done in
// firmware/main/homedeck.cpp's app_main(), ahead of Wi-Fi bring-up).
class FirmwareSecretStore : public SecretStore {
public:
    FirmwareSecretStore() = default;

    bool Set(const std::string& ns, const std::string& key, const std::string& value) override;
    std::optional<std::string> Get(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;
};

}  // namespace homedeck
