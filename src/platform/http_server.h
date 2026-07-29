#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace homedeck {

// A hard ceiling both backends reject a request against (via its
// Content-Length header) before allocating any buffer for the body -
// an attacker-controlled Content-Length shouldn't be able to force a
// large allocation attempt just by claiming one. Comfortably above the
// largest legitimate body this project has (a ~4MB OTA image - see
// core/ota_gate.h's own tighter, partition-sized check, which still
// applies on top of this for that specific endpoint).
constexpr size_t kMaxHttpRequestBodyBytes = 8 * 1024 * 1024;

enum class HttpMethod { kGet, kPost };

struct HttpRequest {
    std::string path;
    std::string query;
    std::string body;
    // Only the Cookie header is exposed, not a generic header map -
    // AdminAuthService's session cookie (see core/admin_auth_service.h)
    // is the only consumer so far, and esp_http_server has no "list all
    // headers" API, only per-name lookup, so a generic enumerable map
    // isn't implementable on firmware without accumulating header names
    // it has no real use for yet. Raw Cookie-header format (RFC 6265),
    // not parsed into individual cookies - the one caller that needs a
    // specific cookie value parses it out itself.
    std::optional<std::string> cookie_header;
};

struct HttpResponse {
    int status_code = 200;
    std::string content_type = "text/plain";
    std::string body;
    // Extra headers beyond Content-Type/Content-Length, which both
    // backends already send unconditionally - e.g. Set-Cookie for
    // AdminAuthService's session cookie. (name, value) pairs rather than
    // raw "Name: value" lines since esp_http_server's httpd_resp_set_hdr()
    // wants them separately.
    std::vector<std::pair<std::string, std::string>> extra_headers;
};

// The embedded HTTP server behind the Web Management UI
// (docs/architecture/web-ui.md) - esp_http_server on firmware, civetweb
// on the simulator (see ADR-0002's "Embedded web/WebSocket server"
// decision for why both, not one). No WebSocket support here - nothing
// yet needs the live-update path ADR-0002 also names, and that path has
// its own unresolved dispatch-safety question for civetweb; adding it
// ahead of a real consumer would be exactly the kind of speculative
// design this project's own precedent (ADR-0003, core.md) rejects.
class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    virtual ~HttpServer() = default;

    // Must be called before Start().
    virtual void RegisterHandler(HttpMethod method, const std::string& path, Handler handler) = 0;

    virtual bool Start(uint16_t port) = 0;
    virtual void Stop() = 0;
};

}  // namespace homedeck
