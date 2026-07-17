#pragma once

#include "platform/cache_store.h"
#include "platform/settings_store.h"

#include <optional>
#include <string>

namespace homedeck {

// A value read back from Storage, paired with the schema version it was
// written under - see core.md's Configuration responsibility ("every
// persisted blob carries a schema version field from the start"). Storage
// itself has no opinion on what a version means or how to migrate between
// them ("migration logic deferred until a real breaking change exists to
// migrate from") - callers that care compare schema_version themselves.
struct VersionedValue {
    int schema_version;
    std::string value;
};

// Fulfils both of core.md's named-but-unimplemented "Configuration" and
// "Storage" responsibilities: reading/writing settings, backed by the
// three-tier physical split from ADR-0012
// (docs/decisions/ADR-0012-storage-tiers.md). Only two tiers are wired up
// here - NVS-backed Settings (small, frequently-read) and FAT-backed
// Cache (larger, structured) - microSD is deferred entirely; ADR-0012
// scopes it to extended log archival, which has no consumer yet. Encryption
// is deferred too (see docs/decisions/ADR-0010-secret-storage.md and the
// M2 Configuration-service roadmap item) - both tiers are currently plain.
//
// Per-module namespacing (ADR-0012's "enforced by the service itself, not
// by convention") comes from requiring module_id on every call and
// passing it straight through to the underlying store, rather than
// trusting callers to prefix their own keys.
class Storage {
public:
    Storage(SettingsStore& settings_store, CacheStore& cache_store);

    bool SetSetting(const std::string& module_id, const std::string& key, int schema_version,
                     const std::string& value);
    std::optional<VersionedValue> GetSetting(const std::string& module_id, const std::string& key);
    bool EraseSetting(const std::string& module_id, const std::string& key);

    bool WriteCache(const std::string& module_id, const std::string& key, int schema_version,
                     const std::string& value);
    std::optional<VersionedValue> ReadCache(const std::string& module_id, const std::string& key);
    bool EraseCache(const std::string& module_id, const std::string& key);

private:
    SettingsStore& settings_store_;
    CacheStore& cache_store_;
};

}  // namespace homedeck
