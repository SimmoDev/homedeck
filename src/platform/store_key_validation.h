#pragma once

#include <string>

namespace homedeck {

// SettingsStore/CacheStore/SecretStore's `ns`/`key` map directly onto a
// filesystem path segment (host, and FirmwareCacheStore's FAT-partition
// path) or an NVS namespace/key string (FirmwareSettingsStore/
// FirmwareSecretStore) - every backend must reject a segment that could
// escape the intended directory or namespace, not just enforce a length
// limit. Empty, "." and ".." are rejected outright, and so is any embedded
// '/' or '\', since std::filesystem's path-append operator treats an
// embedded separator as introducing further path components rather than a
// literal character.
inline bool IsValidStoreSegment(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == "..") {
        return false;
    }
    return segment.find_first_of("/\\") == std::string::npos;
}

}  // namespace homedeck
