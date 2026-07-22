#pragma once

#include <optional>
#include <string>

namespace homedeck {

// Shared NVS blob read/write/erase helpers behind FirmwareSecretStore and
// FirmwareSettingsStore - both map (ns, key) onto an NVS blob and differ
// only in the log tag used for failures, so the actual NVS calls are
// factored out rather than duplicated (mirroring
// platform/host/file_backed_store.h's role for the host-side stores).
bool NvsSetBlob(const char* tag, const std::string& ns, const std::string& key, const std::string& value);
std::optional<std::string> NvsGetBlob(const char* tag, const std::string& ns, const std::string& key);
// No log tag - unlike Set/Get, neither original implementation logged an
// erase failure.
bool NvsEraseBlob(const std::string& ns, const std::string& key);

}  // namespace homedeck
