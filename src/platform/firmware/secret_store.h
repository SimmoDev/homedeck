#pragma once

#include "platform/secret_store.h"

namespace homedeck {

// Wraps ESP-IDF's NVS API against its own dedicated `secrets` partition -
// see docs/decisions/ADR-0027-secret-store-partition-separation.md.
// Plain (unencrypted) storage by design at this project stage - see
// docs/decisions/ADR-0018-staged-security-hardening.md for the staged
// security model that decides when NVS encryption activates.
//
// Physically separate from FirmwareSettingsStore's partition (unlike
// before ADR-0027, and still unlike HostSecretStore, which uses a
// separate directory within the same host filesystem) - a secret can no
// longer be read or enumerated through FirmwareSettingsStore's own
// calls, regardless of (ns, key) collisions. `ns`/`key` are still capped
// at 15 characters (NVS_KEY_NAME_MAX_SIZE - 1), same as
// FirmwareSettingsStore.
//
// Requires nvs_flash_init_partition(kPartitionName) to have already been
// called (already done in firmware/main/homedeck.cpp's app_main(),
// alongside the default partition's own nvs_flash_init(), ahead of Wi-Fi
// bring-up).
class FirmwareSecretStore : public SecretStore {
public:
    // Public so firmware/main/homedeck.cpp's InitNvs() can initialize
    // this exact partition by name too, rather than duplicating the
    // literal in a second place where it could silently drift apart.
    static constexpr char kPartitionName[] = "secrets";

    FirmwareSecretStore() = default;

    bool Set(const std::string& ns, const std::string& key, const std::string& value) override;
    std::optional<std::string> Get(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;
};

}  // namespace homedeck
