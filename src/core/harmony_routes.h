#pragma once

#include "core/admin_auth_service.h"
#include "core/harmony_connection.h"
#include "platform/http_server.h"

namespace homedeck {

// Registers:
// - GET /api/harmony/status - HarmonyConnection::Snapshot() as JSON
//   (connection state, device/activity list once fetched), for the Web
//   UI's Harmony settings page to show alongside the hub-address field.
// - POST /api/harmony/reconnect - calls HarmonyConnection::TriggerReconnect()
//   so the settings page's save flow can ask for an immediate (re)connect
//   attempt instead of waiting out whatever backoff delay is in progress -
//   same shape as core/weather_routes.h's POST /api/weather/refresh.
//
// The hub address itself has no dedicated endpoint here - it's stored
// under (module="harmony", key="hub_host") through the existing generic
// POST/GET /api/settings API (core/settings_routes.h), the same way
// weather's latitude/longitude are. Both admin-only. Must be called
// before server.Start(), per HttpServer's own contract.
void RegisterHarmonyRoutes(HttpServer& server, HarmonyConnection& harmony_connection, AdminAuthService& auth);

}  // namespace homedeck
