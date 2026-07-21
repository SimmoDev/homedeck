#pragma once

#include "core/storage.h"
#include "platform/time_source.h"

#include <string>

namespace homedeck {

// Distinct from ESP-IDF's ESP_LOGI/ESP_LOGE, which stay in use for raw
// boot-time console output - this is Core's persisted, structured,
// leveled log, per docs/decisions/ADR-0019-structured-logging.md.
enum class LogLevel { kDebug, kInfo, kWarning, kError };

// Structured, leveled logging, per core.md's Logging responsibility and
// docs/decisions/ADR-0019-structured-logging.md for the format/rotation/
// storage design this implements. Built entirely on the existing
// Storage/CacheStore API (the same one crash_diagnostics.cpp already
// uses) rather than a new platform interface - see that ADR for why a
// second, independent FAT mount isn't needed. Target-agnostic: no
// firmware-only mechanism, so the simulator uses this exact
// implementation rather than mock data.
class Logger {
public:
    // max_log_file_bytes defaults to the real production cap (see
    // ADR-0019); overridable so tests can trigger rotation in a handful
    // of calls instead of the hundreds it'd take to actually cross 64KB.
    explicit Logger(Storage& storage, TimeSource& time_source, size_t max_log_file_bytes = 64 * 1024);

    void Log(LogLevel level, const std::string& component, const std::string& message);

    // Rotated + current entries, oldest first, as a JSON array (each
    // stored line is already valid JSON, so this joins them rather than
    // parsing and re-serializing).
    std::string ReadAll() const;

private:
    Storage& storage_;
    TimeSource& time_source_;
    size_t max_log_file_bytes_;
};

}  // namespace homedeck
