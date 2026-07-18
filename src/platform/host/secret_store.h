#pragma once

#include "platform/secret_store.h"

#include <filesystem>

namespace homedeck {

// Real, disk-backed - mirrors HostSettingsStore, kept as a distinct
// type/directory ("secrets", not "settings") so the two never collide on
// disk even when a module reuses the same ns/key pair across both
// stores - see
// docs/decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface.
class HostSecretStore : public SecretStore {
public:
    explicit HostSecretStore(std::filesystem::path root_dir);

    bool Set(const std::string& ns, const std::string& key, const std::string& value) override;
    std::optional<std::string> Get(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;

private:
    std::filesystem::path PathFor(const std::string& ns, const std::string& key) const;

    std::filesystem::path root_dir_;
};

}  // namespace homedeck
