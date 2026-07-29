#pragma once

#include <optional>
#include <string>
#include <vector>

namespace homedeck {

// One stored entry, as returned by ListAll() - see its own comment for
// why this exists only on SettingsStore, not SecretStore. `ns` is
// deliberately not named `module_id` to match core/storage.h's
// SettingEntry: this interface is generic key-value storage with no
// concept of "module" of its own (that's a Core-level idea layered on
// top by Storage, which is also the only thing that ever calls
// module_id a module_id) - `ns` names what this layer actually knows
// it's namespacing by, nothing more.
struct SettingsEntry {
    std::string ns;
    std::string key;
    std::string value;
};

// Small, namespaced key-value storage - the NVS tier of ADR-0012's
// three-tier split (docs/decisions/ADR-0012-storage-tiers.md). `ns` maps
// directly onto NVS's own namespace concept on firmware, so per-module
// isolation falls out of the existing mechanism rather than needing
// prefix-string enforcement elsewhere. Small and virtual for the same
// reason as BatteryReader/TimeSource: a simple, infrequently-called
// reader/writer, not a performance-sensitive primitive.
class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    virtual bool Set(const std::string& ns, const std::string& key, const std::string& value) = 0;
    virtual std::optional<std::string> Get(const std::string& ns, const std::string& key) = 0;
    virtual bool Erase(const std::string& ns, const std::string& key) = 0;

    // Every stored entry across every namespace - the settings/backup
    // API's enumeration primitive (docs/decisions/ADR-0023-settings-backup-api.md).
    // Deliberately not mirrored on SecretStore: on firmware both stores
    // share the same physical NVS partition (see
    // platform/firmware/secret_store.h's own comment), so the absence of
    // this method there - plus Storage's reserved-key guard - is what
    // keeps secrets out of a generic listing/backup, not a convention
    // callers have to remember.
    virtual std::vector<SettingsEntry> ListAll() = 0;
};

}  // namespace homedeck
