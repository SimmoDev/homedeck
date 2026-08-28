#pragma once

#include "core/admin_auth_service.h"
#include "core/kodi_client.h"
#include "platform/http_server.h"

namespace homedeck {

// Registers, mirroring core/harmony_routes.h:
// - GET /api/kodi/status - KodiClient::Snapshot() as JSON (connection
//   state, resolved host, discovered instances, app volume/mute, and
//   what's playing), for the Web UI's Kodi settings page.
// - POST /api/kodi/reconnect - KodiClient::TriggerReconnect(), so the
//   settings save flow gets an immediate (re)connect instead of waiting
//   out the current backoff/reconcile delay.
//
// The `host` override and `instance_uuid` selection have no dedicated
// endpoints - both go through the generic POST/GET /api/settings API
// (module "kodi"), the same way Harmony's hub_host does. Admin-only.
// Must be called before server.Start().
void RegisterKodiRoutes(HttpServer& server, KodiClient& kodi_client, AdminAuthService& auth);

}  // namespace homedeck
