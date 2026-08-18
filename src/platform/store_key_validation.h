#pragma once

#include <string>

namespace homedeck {

// NVS namespace/key strings are capped at 15 characters on firmware
// (NVS_KEY_NAME_MAX_SIZE - 1) - HostSettingsStore/HostSecretStore/
// HostCacheStore's file-per-key storage has no equivalent limit of its
// own, so without enforcing this here too, a too-long ns/key would
// silently fail on firmware while working fine on the simulator. Applying
// it uniformly, rather than leaving it to each call site to remember (see
// AdminAuthService::kPasswordKey's own static_assert, still worth keeping
// as a compile-time check for that one hardcoded constant specifically),
// is what ADR-0012 means by storage constraints being enforced by the
// service itself, not by convention.
inline constexpr size_t kMaxStoreSegmentLength = 15;

// SettingsStore/CacheStore/SecretStore's `ns`/`key` map directly onto a
// filesystem path segment (host, and FirmwareCacheStore's FAT-partition
// path) or an NVS namespace/key string (FirmwareSettingsStore/
// FirmwareSecretStore) - every backend must reject a segment that could
// escape the intended directory or namespace, not just enforce a length
// limit. Empty, "." and ".." are rejected outright, and so is any embedded
// '/' or '\', since std::filesystem's path-append operator treats an
// embedded separator as introducing further path components rather than a
// literal character. A segment can arrive here from parsed JSON (e.g.
// settings_routes.cpp's module/key fields), where a Unicode NUL escape in
// the request body produces a std::string with an embedded NUL byte that
// the C string it's eventually passed as (nvs_open()/nvs_set_*'s
// namespace/key arguments on firmware) would silently truncate at - two
// segments that look distinct as std::string could then collide once
// truncated to the same C string, so this is rejected here too, not just
// the path separators above.
inline bool IsValidStoreSegment(const std::string& segment) {
    if (segment.empty() || segment == "." || segment == ".." || segment.size() > kMaxStoreSegmentLength) {
        return false;
    }
    return segment.find_first_of("/\\") == std::string::npos && segment.find(static_cast<char>(0)) == std::string::npos;
}

}  // namespace homedeck
