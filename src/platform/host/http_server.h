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
    bool Start(uint16_t port) override;
    void Stop() override;

private:
    // civetweb's mg_set_request_handler() registers by URI only, not
    // method (unlike esp_http_server's httpd_uri_t) - this dispatches to
    // the right (method, path) handler internally so RegisterHandler()
    // can still offer per-method registration.
    static int DispatchTrampoline(mg_connection* conn, void* cbdata);
    int Dispatch(mg_connection* conn);

    mg_context* ctx_ = nullptr;
    std::map<std::pair<HttpMethod, std::string>, Handler> handlers_;
};

}  // namespace homedeck
