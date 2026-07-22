#pragma once

#include "core/admin_auth_service.h"
#include "core/weather_provider.h"
#include "platform/http_client.h"
#include "platform/http_server.h"

namespace homedeck {

// Registers:
// - GET /api/weather/geocode?query=<name> - a thin, admin-only proxy to
//   Open-Meteo's geocoding API (place name -> candidate latitude/
//   longitude), kept device-side rather than a direct browser->Open-Meteo
//   call so the Web UI stays a client of the device's own API surface,
//   consistent with every other endpoint here - see
//   docs/architecture/networking.md.
// - POST /api/weather/refresh - calls OpenMeteoWeatherProvider::TriggerPoll()
//   so the Web UI's weather settings save flow can ask for an immediate
//   fetch (see weather_provider.h) instead of the dashboard silently
//   waiting out the rest of the real poll interval after a location
//   change.
// Both admin-only. Must be called before server.Start(), per
// HttpServer's own contract.
void RegisterWeatherRoutes(HttpServer& server, HttpClient& http_client, OpenMeteoWeatherProvider& weather_provider,
                            AdminAuthService& auth);

}  // namespace homedeck
