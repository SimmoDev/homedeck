#pragma once

#include "core/admin_auth_service.h"
#include "core/logger.h"
#include "core/storage.h"
#include "platform/battery_reader.h"
#include "platform/http_server.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace homedeck {

// Reads the raw core dump bytes if one is present, nullopt otherwise -
// injected per target since the actual source is platform-specific
// (firmware: real flash via esp_core_dump_image_get(); simulator: a
// mock stub, since ESP-IDF's core dump mechanism doesn't exist on host -
// see docs/architecture/diagnostics.md's "Firmware-only mechanism" note).
using CoreDumpReader = std::function<std::optional<std::string>()>;

// Heap figures reported by GET /api/diagnostics. Internal RAM, not total
// free heap, decides whether a new task or connection can start (task
// stacks cannot come from PSRAM) - see docs/architecture/diagnostics.md#memory
// and docs/decisions/ADR-0031-internal-ram-budget.md.
struct MemoryStats {
    size_t internal_free_bytes = 0;
    size_t internal_largest_block_bytes = 0;
    size_t internal_low_water_bytes = 0;  // lowest internal free size since boot
    size_t psram_free_bytes = 0;
};

// Injected per target, like CoreDumpReader: firmware reads ESP-IDF's heap
// caps; the simulator reports fixed mock values.
using MemoryStatsReader = std::function<MemoryStats()>;

// Registers GET /api/diagnostics (reset reason + core dump presence,
// read from Storage - the same "core"/"reset_reason" and
// "core"/"has_core_dump" keys firmware/main/crash_diagnostics.cpp
// writes every boot - plus live battery percent, external-power, and
// battery-presence state from `battery_reader` and heap figures from
// `read_memory_stats`, useful for confirming
// charging/USB-C/no-battery detection behavior without a serial
// connection, see docs/architecture/hardware.md#power),
// GET /api/diagnostics/coredump
// (the raw core dump file, downloadable for off-device analysis, not
// decoded on-device - see
// docs/decisions/ADR-0013-crash-and-reboot-diagnostics.md), and
// GET /api/diagnostics/logs (structured log entries from `logger` as a
// JSON array - see docs/decisions/ADR-0019-structured-logging.md).
// All three require an authenticated session via auth.RequireAuth() -
// Web UI diagnostics is admin-only, not a public surface. Must be
// called before server.Start(), per HttpServer's own contract.
void RegisterDiagnosticsRoutes(HttpServer& server, Storage& storage, AdminAuthService& auth,
                                BatteryReader& battery_reader, Logger& logger, CoreDumpReader read_core_dump,
                                MemoryStatsReader read_memory_stats);

}  // namespace homedeck
