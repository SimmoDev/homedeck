#include "core/logger.h"

#include "third_party/nlohmann/json.hpp"

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
    : storage_(storage), time_source_(time_source), max_log_file_bytes_(max_log_file_bytes) {}

void Logger::Log(LogLevel level, const std::string& component, const std::string& message) {
    nlohmann::json entry = {
        {"timestamp", std::chrono::system_clock::to_time_t(time_source_.Now())},
        {"level", LevelToString(level)},
        {"component", component},
        {"message", message},
    };
    std::string line = entry.dump() + "\n";

    auto current = storage_.ReadCache(kModuleId, kCurrentKey);
    std::string previous = current.has_value() ? current->value : std::string();
    std::string updated = previous + line;

    if (updated.size() > max_log_file_bytes_) {
        // The content before this new line becomes the rotated backup
        // (overwriting any earlier one); this new line starts the fresh
        // current blob - see ADR-0019's size-based rotation decision.
        storage_.WriteCache(kModuleId, kRotatedKey, kSchemaVersion, previous);
        storage_.WriteCache(kModuleId, kCurrentKey, kSchemaVersion, line);
    } else {
        storage_.WriteCache(kModuleId, kCurrentKey, kSchemaVersion, updated);
    }
}

std::string Logger::ReadAll() const {
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

}  // namespace homedeck
