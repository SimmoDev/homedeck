#include "ui/time_format.h"

#include <ctime>

namespace homedeck {

void FormatLocalTime(std::chrono::system_clock::time_point time, const char* format, char* buffer, size_t size) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buffer, size, format, &tm);
}

}  // namespace homedeck
