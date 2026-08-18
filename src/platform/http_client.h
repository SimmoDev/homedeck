#pragma once

#include <string>
#include <utility>
#include <vector>

namespace homedeck {

// Mirrors platform/http_server.h's kMaxHttpRequestBodyBytes for the
// inbound-request side - this is the outbound-response side. Every real
// response this project's callers see (Harmony's hub config, Open-Meteo's
// weather JSON) is at most a few hundred KB; 1 MiB is a generous multiple
// of that, not a tight fit. Both HostHttpClient and FirmwareHttpClient
// enforce this while accumulating a response body, since Harmony's own
// transport has no authentication (ADR-0029) - a rogue LAN device could
// otherwise return an arbitrarily large body within the existing
// timeout, growing the response string without bound.
constexpr size_t kMaxHttpResponseBodyBytes = 1024 * 1024;

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

// Outbound HTTP(S) client - see docs/architecture/networking.md. Small
// and virtual for the same reason as NetworkStatus/BatteryReader/
// CacheStore: a simple, infrequently-called operation where mockability
// matters directly, not a performance-sensitive primitive.
class HttpClient {
public:
    virtual ~HttpClient() = default;

    virtual HttpClientResponse Get(const std::string& url) = 0;

    // Always sends Content-Type: application/json - every current/
    // planned caller posts a JSON body; add a content-type parameter if
    // a future caller genuinely needs something else, rather than
    // generalizing ahead of that need. extra_headers is (name, value)
    // pairs sent in addition to Content-Type - added for
    // HarmonyConnection's hub handshake, which a live probe against the
    // reference hub confirmed requires an Origin header the hub otherwise
    // rejects with a 400 (see ADR-0029); kept generic here (not a
    // hardcoded Origin) rather than baking Harmony-specific knowledge
    // into this shared platform interface, per ADR-0003's "Core stays
    // thin" boundary.
    virtual HttpClientResponse Post(const std::string& url, const std::string& json_body,
                                     const std::vector<std::pair<std::string, std::string>>& extra_headers = {}) = 0;
};

}  // namespace homedeck
