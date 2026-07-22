#pragma once

#include <string>

namespace homedeck {

// Named HttpClientResponse, not HttpResponse - platform/http_server.h
// already defines homedeck::HttpResponse for the server side (a
// different shape: status/content-type/body/extra-headers), and both
// headers can end up included in the same translation unit.
struct HttpClientResponse {
    // false on a network/timeout error - no response was received at
    // all. A non-2xx HTTP status is still success=true; callers check
    // status_code themselves, the same split curl/esp_http_client both
    // make between transport failure and HTTP-level outcome.
    bool success;
    int status_code;
    std::string body;
};

// Outbound HTTP(S) client - see docs/architecture/networking.md. GET
// only, no headers: the only thing Open-Meteo's weather/geocoding
// endpoints need. Small and virtual for the same reason as
// NetworkStatus/BatteryReader/CacheStore: a simple, infrequently-called
// operation where mockability matters directly, not a performance-
// sensitive primitive.
class HttpClient {
public:
    virtual ~HttpClient() = default;

    virtual HttpClientResponse Get(const std::string& url) = 0;
};

}  // namespace homedeck
