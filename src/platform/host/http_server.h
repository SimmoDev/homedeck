#pragma once

#include "platform/http_server.h"

#include <map>
#include <utility>

struct mg_context;
struct mg_connection;

namespace homedeck {

// Real civetweb server, not a mock - see platform/http_server.h. Uses
// civetweb's plain C API directly (mg_start/mg_set_request_handler/
// mg_read/mg_send_http_ok), not its C++ wrapper (CivetServer) - this
// class is already the abstraction callers see, so the wrapper would be
// an extra dependency for no real benefit.
class HostHttpServer : public HttpServer {
public:
    HostHttpServer();
    ~HostHttpServer() override;

    HostHttpServer(const HostHttpServer&) = delete;
    HostHttpServer& operator=(const HostHttpServer&) = delete;

    void RegisterHandler(HttpMethod method, const std::string& path, Handler handler) override;
    // Pass 0 to bind a kernel-assigned free port, then read it back with
    // BoundPort() - the hermetic pattern tests use so parallel/repeated
    // runs don't collide on a fixed port. A non-zero port behaves as
    // before.
    bool Start(uint16_t port) override;
    void Stop() override;

    // The port Start() actually bound (0 before a successful Start or
    // after Stop). Host-only, not on the HttpServer interface - firmware
    // always binds a fixed port and nothing there needs to ask.
    uint16_t BoundPort() const { return bound_port_; }

private:
    // civetweb's mg_set_request_handler() registers by URI only, not
    // method (unlike esp_http_server's httpd_uri_t) - this dispatches to
    // the right (method, path) handler internally so RegisterHandler()
    // can still offer per-method registration.
    static int DispatchTrampoline(mg_connection* conn, void* cbdata);
    int Dispatch(mg_connection* conn);

    mg_context* ctx_ = nullptr;
    uint16_t bound_port_ = 0;
    std::map<std::pair<HttpMethod, std::string>, Handler> handlers_;
};

}  // namespace homedeck
