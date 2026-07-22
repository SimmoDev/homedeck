#include "core/storage.h"

#include "core/admin_auth_service.h"

#include <charconv>

namespace homedeck {

namespace {

// SettingsStore and SecretStore share the same physical NVS
// namespace-per-module_id on firmware (no partition-level separation
// yet - see docs/decisions/ADR-0023-settings-backup-api.md), so a
// generic settings write/list could otherwise reach the admin password
// hash through the wrong door. Interim guard until that partition split
// lands; extend this whenever a new SecretStore key is introduced.
bool IsReservedForSecrets(const std::string& module_id, const std::string& key) {
    return module_id == AdminAuthService::kModuleId && key == AdminAuthService::kPasswordKey;
}

// A schema-versioned envelope, deliberately not JSON - Storage doesn't
// need to understand what's inside a value, only carry a version
// alongside it (see storage.h). A plain "<version>\n<value>" prefix keeps
// this dependency-free and human-readable in a serial log or on-device
// file browse, which a binary header wouldn't be.
std::string Encode(int schema_version, const std::string& value) {
    return std::to_string(schema_version) + "\n" + value;
}

std::optional<VersionedValue> Decode(const std::string& raw) {
    std::string::size_type separator = raw.find('\n');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    // std::from_chars, not std::stoi - firmware builds with exceptions
    // disabled (ESP-IDF's default), and stoi's invalid-input path is
    // exception-only.
    int schema_version = 0;
    const char* begin = raw.data();
    const char* end = raw.data() + separator;
    auto [ptr, ec] = std::from_chars(begin, end, schema_version);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return VersionedValue{schema_version, raw.substr(separator + 1)};
}

// Shared by GetSetting/GetSecret/ReadCache - identical "no value stored"
// vs "decode what's there" handling, differing only in which store's Get/
// Read produced raw.
std::optional<VersionedValue> DecodeIfPresent(const std::optional<std::string>& raw) {
    if (!raw.has_value()) {
        return std::nullopt;
    }
    return Decode(*raw);
}

}  // namespace

Storage::Storage(SettingsStore& settings_store, CacheStore& cache_store, SecretStore& secret_store)
    : settings_store_(settings_store), cache_store_(cache_store), secret_store_(secret_store) {}

bool Storage::SetSetting(const std::string& module_id, const std::string& key, int schema_version,
                          const std::string& value) {
    if (IsReservedForSecrets(module_id, key)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_store_.Set(module_id, key, Encode(schema_version, value));
}

std::optional<VersionedValue> Storage::GetSetting(const std::string& module_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DecodeIfPresent(settings_store_.Get(module_id, key));
}

bool Storage::EraseSetting(const std::string& module_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_store_.Erase(module_id, key);
}

std::vector<SettingEntry> Storage::ListAllSettings() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SettingEntry> entries;
    for (const SettingsEntry& raw : settings_store_.ListAll()) {
        if (IsReservedForSecrets(raw.ns, raw.key)) {
            continue;
        }
        std::optional<VersionedValue> decoded = Decode(raw.value);
        if (!decoded.has_value()) {
            continue;
        }
        entries.push_back({raw.ns, raw.key, decoded->schema_version, decoded->value});
    }
    return entries;
}

bool Storage::SetSecret(const std::string& module_id, const std::string& key, int schema_version,
                         const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return secret_store_.Set(module_id, key, Encode(schema_version, value));
}

std::optional<VersionedValue> Storage::GetSecret(const std::string& module_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DecodeIfPresent(secret_store_.Get(module_id, key));
}

bool Storage::EraseSecret(const std::string& module_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return secret_store_.Erase(module_id, key);
}

bool Storage::WriteCache(const std::string& module_id, const std::string& key, int schema_version,
                          const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_store_.Write(module_id, key, Encode(schema_version, value));
}

std::optional<VersionedValue> Storage::ReadCache(const std::string& module_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return DecodeIfPresent(cache_store_.Read(module_id, key));
}

bool Storage::EraseCache(const std::string& module_id, const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_store_.Erase(module_id, key);
}

}  // namespace homedeck
