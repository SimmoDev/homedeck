#pragma once

#include "core/admin_auth_service.h"
#include "core/storage.h"
#include "platform/http_server.h"

#include <functional>
#include <string>

namespace homedeck {

// Called only when (module="core", key="device_name") is written via
// POST /api/settings, before the new value is persisted - lets the
// caller validate it and apply it immediately (firmware: checks RFC
// 1035/6763 hostname rules and re-announces mDNS live, no reboot
// needed; simulator: omitted entirely, nothing to apply). Returning
// false makes the route respond 400 rather than persisting a value that
// was rejected. Not consulted by POST /api/backup/restore's replay -
// see docs/decisions/ADR-0023-settings-backup-api.md for why that's an
// accepted simplification, not an oversight.
using DeviceNameChangedFn = std::function<bool(const std::string& value)>;

// Registers GET/POST /api/settings, POST /api/settings/erase,
// GET /api/backup, and POST /api/backup/restore - see
// docs/architecture/web-ui.md and
// docs/decisions/ADR-0023-settings-backup-api.md for the endpoint
// shapes and the reserved-key guard (Storage::SetSetting/ListAllSettings)
// that keeps the admin password hash out of this generic surface. All
// admin-only via auth.RequireAuth(). Must be called before
// server.Start(), per HttpServer's own contract.
void RegisterSettingsRoutes(HttpServer& server, Storage& storage, AdminAuthService& auth,
                             DeviceNameChangedFn on_device_name_changed = nullptr);

}  // namespace homedeck
