#pragma once

#include "platform/cache_store.h"
#include "platform/secret_store.h"
#include "platform/settings_store.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

// One entry from ListAllSettings() - the settings/backup API's
// enumeration primitive (docs/decisions/ADR-0023-settings-backup-api.md).
// `module_id` is platform/settings_store.h's SettingsEntry::ns renamed,
// not a different value - deliberately, since this is the layer that
// actually knows the string means "which module," a concept
// SettingsStore itself has no notion of (see its own comment on `ns`).
struct SettingEntry {
    std::string module_id;
    std::string key;
    int schema_version;
    std::string value;
};

// Fulfils both of core.md's named-but-unimplemented "Configuration" and
// "Storage" responsibilities: reading/writing settings, backed by the
// three-tier physical split from ADR-0012
// (docs/decisions/ADR-0012-storage-tiers.md). Three stores are wired up
// here - NVS-backed Settings and Secrets (small, frequently-read; kept
// as physically-separate call paths so secrets are structurally distinct
// from general config, see
// docs/decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface)
// and FAT-backed Cache (larger, structured) - microSD is deferred
// entirely; ADR-0012 scopes it to extended log archival, which has no
// consumer yet. NVS encryption itself is deferred (see
// docs/decisions/ADR-0018-staged-security-hardening.md) - all tiers are
// currently plain.
//
// Per-module namespacing (ADR-0012's "enforced by the service itself, not
// by convention") comes from requiring module_id on every call and
// passing it straight through to the underlying store, rather than
// trusting callers to prefix their own keys.
//
// Thread-safe: every method locks a single internal mutex_ - genuinely
// needed, not defensive. app_main's own boot-sequence thread, the Web
// UI's httpd worker thread, and OpenMeteoWeatherProvider's background
// poll Task (core/weather_provider.h) all call into the same Storage
// instance, none coordinated with each other.
class Storage {
public:
    Storage(SettingsStore& settings_store, CacheStore& cache_store, SecretStore& secret_store);

    // Both check a small reserved-key guard against the admin password's
    // exact (module_id, key) - see storage.cpp. SettingsStore and
    // SecretStore now live on physically separate NVS partitions on
    // firmware (docs/decisions/ADR-0027-secret-store-partition-separation.md),
    // so this is a second-layer safeguard against a confusing namespace
    // collision, not the only thing preventing an overwrite through the
    // wrong door. SetSetting returns false for that key; ListAllSettings
    // silently excludes it.
    [[nodiscard]] bool SetSetting(const std::string& module_id, const std::string& key, int schema_version,
                                   const std::string& value);
    std::optional<VersionedValue> GetSetting(const std::string& module_id, const std::string& key);
    [[nodiscard]] bool EraseSetting(const std::string& module_id, const std::string& key);

    // Every setting across every module - the settings/backup API's
    // enumeration primitive. Entries that fail to decode (defensive -
    // NVS could in principle hold something not written through this
    // class's own Encode() format) are silently skipped rather than
    // surfaced as partial/malformed results.
    std::vector<SettingEntry> ListAllSettings();

    // Secrets excluded from config export/backups - see
    // docs/decisions/ADR-0012-storage-tiers.md#decision-backup-delivery.
    // Callers must not reuse a (module_id, key) pair already used through
    // SetSetting/WriteCache for the same module - see
    // FirmwareSecretStore's own comment for why that's not structurally
    // prevented on firmware.
    [[nodiscard]] bool SetSecret(const std::string& module_id, const std::string& key, int schema_version,
                                  const std::string& value);
    std::optional<VersionedValue> GetSecret(const std::string& module_id, const std::string& key);
    [[nodiscard]] bool EraseSecret(const std::string& module_id, const std::string& key);

    [[nodiscard]] bool WriteCache(const std::string& module_id, const std::string& key, int schema_version,
                                   const std::string& value);
    std::optional<VersionedValue> ReadCache(const std::string& module_id, const std::string& key);
    [[nodiscard]] bool EraseCache(const std::string& module_id, const std::string& key);

private:
    std::mutex mutex_;
    SettingsStore& settings_store_;
    CacheStore& cache_store_;
    SecretStore& secret_store_;
};

}  // namespace homedeck
