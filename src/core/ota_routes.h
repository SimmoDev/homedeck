#pragma once

#include "core/admin_auth_service.h"
#include "platform/battery_reader.h"
#include "platform/http_server.h"

#include <cstddef>
#include <functional>
#include <string>

namespace homedeck {

// Platform-specific OTA write mechanics, injected the same way
// CoreDumpReader is (see core/diagnostics_routes.h) - firmware drives
// the real esp_ota_* API against flash; the simulator reads the body
// (exercising the same read path) and returns a mocked outcome, per
// docs/architecture/simulator.md's OTA mock description.
struct OtaWriter {
    // The receiving partition's actual capacity, used to reject an
    // oversized image before writing.
    std::function<size_t()> max_image_size;
    // Writes the image and returns true on success. Must not leave a
    // partially-written image bootable on failure.
    std::function<bool(const std::string& image)> write_image;
    // The currently running firmware version string.
    std::function<std::string()> running_version;
};

// Reboots the device. Must not block waiting for the reboot to
// complete - the handler still needs to return so its 200 response is
// actually sent first (firmware: schedules esp_restart() a short delay
// out on a timer/task; simulator: a no-op, doesn't exit the process).
using OtaRebootFn = std::function<void()>;

// Registers GET /api/ota/status, POST /api/ota/upload, and
// POST /api/ota/reboot - see docs/architecture/web-ui.md. All three
// require an authenticated session via auth.RequireAuth(), matching
// diagnostics_routes.h's precedent - OTA is admin-only, not a public
// surface. Must be called before server.Start(), per HttpServer's own
// contract.
//
// POST /api/ota/upload takes the raw firmware image as the full request
// body. Handler::body is already fully buffered in memory by the time
// any handler runs (see platform/http_server.h's Handler contract), so
// the size check against max_image_size() happens against the received
// body, before write_image() is called - not before receiving it.
void RegisterOtaRoutes(HttpServer& server, AdminAuthService& auth, BatteryReader& battery_reader, OtaWriter writer,
                        OtaRebootFn reboot);

}  // namespace homedeck
