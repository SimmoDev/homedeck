#pragma once

#include "core/admin_auth_service.h"
#include "core/storage.h"
#include "platform/http_server.h"

#include <functional>
#include <string>

namespace homedeck {

// Called only when (module="core", key="device_name") is written via
// POST /api/settings, before the new value is persisted - lets the
// caller reject a malformed value (firmware: RFC 1035/6763 hostname
// rules; simulator: omitted entirely, nothing to validate). Returning
// false makes the route respond 400 rather than persisting a value that
// was rejected. Pure - must not apply any live side effect (see
// DeviceNameCommittedFn below for that), since Storage::SetSetting()
// itself can still fail after this returns true, and a side effect
// applied here would then be live while the persisted value still held
// the old one. Not consulted by POST /api/backup/restore's replay - see
// docs/decisions/ADR-0023-settings-backup-api.md for why that's an
// accepted simplification, not an oversight.
using DeviceNameValidateFn = std::function<bool(const std::string& value)>;

// Called only once storage.SetSetting() has actually persisted the new
// device name - the one point it's safe to apply a live side effect that
// can't itself be rolled back (firmware: re-announces mDNS with no
// reboot needed; simulator: omitted, nothing to apply).
using DeviceNameCommittedFn = std::function<void(const std::string& value)>;

// Registers GET/POST /api/settings, POST /api/settings/erase,
// GET /api/backup, and POST /api/backup/restore - see
// docs/architecture/web-ui.md and
// docs/decisions/ADR-0023-settings-backup-api.md for the endpoint
// shapes and the reserved-key guard (Storage::SetSetting/ListAllSettings)
// that keeps the admin password hash out of this generic surface. All
// admin-only via auth.RequireAuth(). Must be called before
// server.Start(), per HttpServer's own contract.
void RegisterSettingsRoutes(HttpServer& server, Storage& storage, AdminAuthService& auth,
                             DeviceNameValidateFn on_device_name_validate = nullptr,
                             DeviceNameCommittedFn on_device_name_committed = nullptr);

}  // namespace homedeck
