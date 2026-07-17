#pragma once

#include "platform/cache_store.h"

#include <filesystem>

namespace homedeck {

// Real, disk-backed - see HostSettingsStore's header comment for why.
// `root_dir` is caller-supplied for the same reason.
class HostCacheStore : public CacheStore {
public:
    explicit HostCacheStore(std::filesystem::path root_dir);

    bool Write(const std::string& ns, const std::string& key, const std::string& content) override;
    std::optional<std::string> Read(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;

private:
    std::filesystem::path PathFor(const std::string& ns, const std::string& key) const;

    std::filesystem::path root_dir_;
};

}  // namespace homedeck
