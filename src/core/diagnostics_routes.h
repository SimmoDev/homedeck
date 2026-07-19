#pragma once

#include "core/admin_auth_service.h"
#include "core/storage.h"
#include "platform/http_server.h"

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

// Registers GET /api/diagnostics (reset reason + core dump presence,
// read from Storage - the same "core"/"reset_reason" and
// "core"/"has_core_dump" keys firmware/main/crash_diagnostics.cpp
// writes every boot) and GET /api/diagnostics/coredump (the raw core
// dump file, downloadable for off-device analysis, not decoded
// on-device - see docs/decisions/ADR-0013-crash-and-reboot-diagnostics.md).
// Both require an authenticated session via auth.RequireAuth() - Web UI
// diagnostics is admin-only, not a public surface. Must be called
// before server.Start(), per HttpServer's own contract.
void RegisterDiagnosticsRoutes(HttpServer& server, Storage& storage, AdminAuthService& auth,
                                CoreDumpReader read_core_dump);

}  // namespace homedeck
