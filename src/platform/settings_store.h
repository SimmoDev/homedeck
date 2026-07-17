#pragma once

#include <optional>
#include <string>

namespace homedeck {

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
};

}  // namespace homedeck
