#include "core/storage.h"

#include <charconv>

namespace homedeck {

namespace {

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

}  // namespace

Storage::Storage(SettingsStore& settings_store, CacheStore& cache_store)
    : settings_store_(settings_store), cache_store_(cache_store) {}

bool Storage::SetSetting(const std::string& module_id, const std::string& key, int schema_version,
                          const std::string& value) {
    return settings_store_.Set(module_id, key, Encode(schema_version, value));
}

std::optional<VersionedValue> Storage::GetSetting(const std::string& module_id, const std::string& key) {
    std::optional<std::string> raw = settings_store_.Get(module_id, key);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    return Decode(*raw);
}

bool Storage::EraseSetting(const std::string& module_id, const std::string& key) {
    return settings_store_.Erase(module_id, key);
}

bool Storage::WriteCache(const std::string& module_id, const std::string& key, int schema_version,
                          const std::string& value) {
    return cache_store_.Write(module_id, key, Encode(schema_version, value));
}

std::optional<VersionedValue> Storage::ReadCache(const std::string& module_id, const std::string& key) {
    std::optional<std::string> raw = cache_store_.Read(module_id, key);
    if (!raw.has_value()) {
        return std::nullopt;
    }
    return Decode(*raw);
}

bool Storage::EraseCache(const std::string& module_id, const std::string& key) {
    return cache_store_.Erase(module_id, key);
}

}  // namespace homedeck
