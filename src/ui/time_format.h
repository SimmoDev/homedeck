#pragma once

#include <chrono>
#include <cstddef>

namespace homedeck {

// localtime_r + strftime into a caller-supplied buffer - shared by every
// UI element that renders a ClockTickEvent's time_point, since each only
// differs in the strftime format string. localtime_r is POSIX; matches
// Linux being the only currently-verified simulator platform (see
// DEVELOPMENT.md).
void FormatLocalTime(std::chrono::system_clock::time_point time, const char* format, char* buffer, size_t size);

}  // namespace homedeck
