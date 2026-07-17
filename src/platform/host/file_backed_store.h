#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace homedeck {

// Shared plain-file read/write/erase helpers behind HostSettingsStore and
// HostCacheStore - both map (ns, key) onto a file path and differ only in
// which subdirectory they use, so the actual I/O is factored out rather
// than duplicated.
bool WriteFile(const std::filesystem::path& path, const std::string& content);
std::optional<std::string> ReadFile(const std::filesystem::path& path);
bool EraseFile(const std::filesystem::path& path);

}  // namespace homedeck
