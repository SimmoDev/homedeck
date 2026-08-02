#include "core/logger.h"

#include "third_party/nlohmann/json.hpp"

#include <future>
#include <iostream>
#include <vector>

namespace homedeck {

namespace {

constexpr const char* kModuleId = "core";
constexpr const char* kCurrentKey = "log_current";
constexpr const char* kRotatedKey = "log_rotated";
constexpr int kSchemaVersion = 1;

const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug:
            return "debug";
        case LogLevel::kInfo:
            return "info";
        case LogLevel::kWarning:
            return "warning";
        case LogLevel::kError:
            return "error";
    }
    return "unknown";
}

// Keeps only whichever suffix of `blob` (a newline-separated run of
// complete JSON lines) fits within max_bytes, dropping the oldest
// entries rather than exceeding the cap - without this, a single batch
// larger than max_log_file_bytes_ on its own (e.g. many Log() calls
// coalesced into one write, per ADR-0020) would become the new current
// blob uncapped, since it's already smaller than `updated` was. Always
// keeps at least the final line intact, even if that one line alone is
// bigger than max_bytes - losing the single most recent entry entirely
// would be worse than a one-off cap overshoot (see logger_test.cpp's
// RotatesOnceSizeThresholdIsExceeded, which deliberately configures a
// cap smaller than one entry's real size, and still expects that entry
// to survive).
std::string TrimToTail(const std::string& blob, size_t max_bytes) {
    if (blob.size() <= max_bytes) {
        return blob;
    }
    std::vector<size_t> line_starts{0};
    for (size_t pos = blob.find('\n'); pos != std::string::npos && pos + 1 < blob.size();
         pos = blob.find('\n', pos + 1)) {
        line_starts.push_back(pos + 1);
    }
    size_t cut = line_starts.back();
    for (auto it = line_starts.rbegin() + 1; it != line_starts.rend(); ++it) {
        if (blob.size() - *it > max_bytes) {
            break;
        }
        cut = *it;
    }
    return blob.substr(cut);
}

// The stored blobs are newline-separated JSON objects, one per Log()
// call - this turns that into comma-separated array elements without
// parsing and re-serializing each one, since every stored line is
// already valid JSON on its own.
std::string JoinLinesAsArrayElements(const std::string& blob) {
    std::string result;
    size_t start = 0;
    while (start < blob.size()) {
        size_t newline = blob.find('\n', start);
        std::string line =
            (newline == std::string::npos) ? blob.substr(start) : blob.substr(start, newline - start);
        if (!line.empty()) {
            if (!result.empty()) {
                result += ',';
            }
            result += line;
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }
    return result;
}

}  // namespace

Logger::Logger(Storage& storage, TimeSource& time_source, size_t max_log_file_bytes)
    : storage_(storage),
      time_source_(time_source),
      max_log_file_bytes_(max_log_file_bytes),
      worker_("logger", [this](std::stop_token stop_token) { WorkerLoop(stop_token); }) {}

// Task's own destructor requests a stop and joins - WorkerLoop() is
// blocked in pending_.Pop(stop_token), which wakes on either a new item
// or the stop request, so no explicit shutdown signal needs to be
// pushed here.
Logger::~Logger() = default;

void Logger::Log(LogLevel level, const std::string& component, const std::string& message) {
    // Timestamp captured now, not whenever WorkerLoop() eventually
    // persists this - callers need the time the event actually
    // happened, not whenever the write got around to running.
    Record record{std::chrono::system_clock::to_time_t(time_source_.Now()), level, component, message};
    pending_.Push(Item{std::move(record), nullptr});
}

std::string Logger::ReadAll() {
    auto signal = std::make_shared<std::promise<void>>();
    std::future<void> done = signal->get_future();
    pending_.Push(Item{std::nullopt, signal});
    done.wait();

    auto rotated = storage_.ReadCache(kModuleId, kRotatedKey);
    auto current = storage_.ReadCache(kModuleId, kCurrentKey);

    std::string rotated_elements = rotated.has_value() ? JoinLinesAsArrayElements(rotated->value) : std::string();
    std::string current_elements = current.has_value() ? JoinLinesAsArrayElements(current->value) : std::string();

    std::string result = "[";
    result += rotated_elements;
    if (!rotated_elements.empty() && !current_elements.empty()) {
        result += ',';
    }
    result += current_elements;
    result += "]";
    return result;
}

void Logger::WorkerLoop(std::stop_token stop_token) {
    while (true) {
        std::optional<Item> first = pending_.Pop(stop_token);
        if (!first.has_value()) {
            return;  // Stop requested, nothing left to wait for.
        }

        std::vector<Record> batch;
        std::vector<std::shared_ptr<std::promise<void>>> flush_signals;
        auto consume = [&](Item&& item) {
            if (item.record.has_value()) {
                batch.push_back(std::move(*item.record));
            }
            if (item.flush_signal) {
                flush_signals.push_back(std::move(item.flush_signal));
            }
        };
        consume(std::move(*first));

        // Drain whatever else is already queued without blocking for
        // more - this is what coalesces a burst of near-simultaneous
        // Log() calls into a single flash write instead of several. See
        // ADR-0020 for why that burst is exactly the case this exists
        // to handle.
        while (std::optional<Item> more = pending_.TryPop()) {
            consume(std::move(*more));
        }

        if (!batch.empty()) {
            WriteBatch(batch);
        }
        // Only after the batch above (everything queued ahead of these
        // signals) has actually been written - FIFO ordering plus
        // strictly-sequential processing here guarantees that.
        for (auto& signal : flush_signals) {
            signal->set_value();
        }
    }
}

void Logger::WriteBatch(const std::vector<Record>& batch) {
    std::string lines;
    for (const auto& record : batch) {
        nlohmann::json entry = {
            {"timestamp", record.timestamp},
            {"level", LevelToString(record.level)},
            {"component", record.component},
            {"message", record.message},
        };
        lines += entry.dump() + "\n";
    }

    auto current = storage_.ReadCache(kModuleId, kCurrentKey);
    std::string previous = current.has_value() ? current->value : std::string();
    std::string updated = previous + lines;

    // WriteCache()'s own backend already logs the low-level I/O reason
    // (partition full/unmounted, etc.) at the console level - this adds
    // the one thing that layer can't know: that this specific batch of
    // log entries is now permanently lost, not just delayed, since
    // there's no retry and the caller (Log(), fire-and-forget by
    // design) has no way to find out on its own.
    if (updated.size() > max_log_file_bytes_) {
        // The content before this batch becomes the rotated backup
        // (overwriting any earlier one); this batch starts the fresh
        // current blob - see ADR-0019's size-based rotation decision.
        // TrimToTail is a no-op unless this one batch is itself larger
        // than the cap.
        if (!storage_.WriteCache(kModuleId, kRotatedKey, kSchemaVersion, previous)) {
            std::cerr << "Logger: failed to persist rotated log backup\n";
        }
        if (!storage_.WriteCache(kModuleId, kCurrentKey, kSchemaVersion,
                                  TrimToTail(lines, max_log_file_bytes_))) {
            std::cerr << "Logger: failed to persist " << batch.size() << " log entr"
                       << (batch.size() == 1 ? "y" : "ies") << " (rotation write)\n";
        }
    } else {
        if (!storage_.WriteCache(kModuleId, kCurrentKey, kSchemaVersion, updated)) {
            std::cerr << "Logger: failed to persist " << batch.size() << " log entr"
                       << (batch.size() == 1 ? "y" : "ies") << "\n";
        }
    }
}

}  // namespace homedeck
