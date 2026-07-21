#pragma once

#include "core/storage.h"
#include "platform/queue.h"
#include "platform/task.h"
#include "platform/time_source.h"

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace homedeck {

// Distinct from ESP-IDF's ESP_LOGI/ESP_LOGE, which stay in use for raw
// boot-time console output - this is Core's persisted, structured,
// leveled log, per docs/decisions/ADR-0019-structured-logging.md and
// ADR-0020 for why persistence runs on a background task rather than
// synchronously inside Log().
enum class LogLevel { kDebug, kInfo, kWarning, kError };

// Structured, leveled logging, per core.md's Logging responsibility.
// Built entirely on the existing Storage/CacheStore API (the same one
// crash_diagnostics.cpp already uses) rather than a new platform
// interface - see ADR-0019 for why a second, independent FAT mount
// isn't needed. Target-agnostic: no firmware-only mechanism, so the
// simulator uses this exact implementation rather than mock data.
class Logger {
public:
    // max_log_file_bytes defaults to the real production cap (see
    // ADR-0019); overridable so tests can trigger rotation in a handful
    // of calls instead of the hundreds it'd take to actually cross 64KB.
    explicit Logger(Storage& storage, TimeSource& time_source, size_t max_log_file_bytes = 64 * 1024);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Never blocks on flash I/O - captures the entry (with a real
    // timestamp, taken now) and hands it to a background task for
    // persistence, batched with whatever else is pending at the time.
    // See ADR-0020: a synchronous flash write here was confirmed to
    // cause a real display glitch when multiple Log() calls landed
    // within the same second during boot (each esp_flash write can
    // transiently disrupt DMA access to the PSRAM-backed display
    // framebuffer). Moving the write off the caller's task doesn't
    // reduce how many flash writes happen on its own - coalescing
    // near-simultaneous calls into one write is what does that, cutting
    // the original three-writes-per-boot case down to one. This
    // measurably reduces the glitch's likelihood but doesn't eliminate
    // it - a single write during active viewing can still glitch on
    // this hardware, a known limitation with no complete fix available
    // yet (see ADR-0020's Context for what was ruled out and why).
    void Log(LogLevel level, const std::string& component, const std::string& message);

    // Rotated + current entries, oldest first, as a JSON array (each
    // stored line is already valid JSON, so this joins them rather than
    // parsing and re-serializing). Blocks until every Log() call made
    // before this one has actually been persisted, so a caller never
    // sees a view that's missing something it already logged.
    std::string ReadAll();

private:
    struct Record {
        long long timestamp;
        LogLevel level;
        std::string component;
        std::string message;
    };
    // A queue item is either a real record to persist, or a flush
    // request (record empty) that WorkerLoop() acknowledges once
    // everything pushed ahead of it has actually been written.
    struct Item {
        std::optional<Record> record;
        std::shared_ptr<std::promise<void>> flush_signal;
    };

    void WorkerLoop(std::stop_token stop_token);
    void WriteBatch(const std::vector<Record>& batch);

    Storage& storage_;
    TimeSource& time_source_;
    size_t max_log_file_bytes_;
    Queue<Item> pending_;
    // Constructed last - starts running WorkerLoop() immediately, which
    // touches every member above, so they must already be initialized.
    Task worker_;
};

}  // namespace homedeck
