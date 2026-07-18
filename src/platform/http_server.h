#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace homedeck {

enum class HttpMethod { kGet, kPost };

struct HttpRequest {
    std::string path;
    std::string query;
    std::string body;
};

struct HttpResponse {
    int status_code = 200;
    std::string content_type = "text/plain";
    std::string body;
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
