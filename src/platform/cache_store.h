#pragma once

#include <optional>
#include <string>

namespace homedeck {

// File-backed, namespaced storage for larger values - the internal flash
// filesystem tier of ADR-0012's three-tier split
// (docs/decisions/ADR-0012-storage-tiers.md): cached dashboard/device-list
// data and rotating logs, unlike SettingsStore's small/sensitive/
// frequently-read NVS tier. `ns` maps onto a per-module subdirectory on
// firmware. Small and virtual for the same reason as SettingsStore.
class CacheStore {
public:
    virtual ~CacheStore() = default;

    virtual bool Write(const std::string& ns, const std::string& key, const std::string& content) = 0;
    virtual std::optional<std::string> Read(const std::string& ns, const std::string& key) = 0;
    virtual bool Erase(const std::string& ns, const std::string& key) = 0;
};

}  // namespace homedeck
