#pragma once

#include <optional>
#include <string>

namespace homedeck {

// Small, namespaced key-value storage for secrets - same shape as
// SettingsStore, kept as a distinct type so call sites are explicit
// about what's routed through it, and so config/backup export code has
// a structural signal to exclude secrets rather than relying on callers
// remembering to (see
// docs/decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface
// and
// docs/decisions/ADR-0012-storage-tiers.md#decision-backup-delivery).
// Currently backed by the same plain NVS storage SettingsStore uses -
// see docs/decisions/ADR-0018-staged-security-hardening.md for when
// that changes; this interface is the one seam that pass touches.
class SecretStore {
public:
    virtual ~SecretStore() = default;

    virtual bool Set(const std::string& ns, const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> Get(const std::string& ns, const std::string& key) = 0;
    virtual bool Erase(const std::string& ns, const std::string& key) = 0;
};

}  // namespace homedeck
