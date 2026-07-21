#pragma once

#include "platform/settings_store.h"

#include <filesystem>

namespace homedeck {

// Real, disk-backed - not a mock. Unlike battery/RTC (no meaningful
// desktop equivalent), a filesystem is a genuine, working stand-in for
// small persisted values, so this exercises real read/write/erase
// behavior in ctest rather than approximating it. `root_dir` is supplied
// by the caller (simulator/main.cpp picks a real scratch directory;
// tests pick a fresh temp directory per test) rather than defaulted
// here, keeping this class free of environment-variable coupling.
class HostSettingsStore : public SettingsStore {
public:
    explicit HostSettingsStore(std::filesystem::path root_dir);

    bool Set(const std::string& ns, const std::string& key, const std::string& value) override;
    std::optional<std::string> Get(const std::string& ns, const std::string& key) override;
    bool Erase(const std::string& ns, const std::string& key) override;
    std::vector<SettingsEntry> ListAll() override;

private:
    std::filesystem::path PathFor(const std::string& ns, const std::string& key) const;

    std::filesystem::path root_dir_;
};

}  // namespace homedeck
