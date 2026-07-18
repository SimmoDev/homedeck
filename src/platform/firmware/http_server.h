#pragma once

#include "platform/http_server.h"

#include "esp_http_server.h"

#include <map>
#include <utility>

namespace homedeck {

// Wraps esp_http_server directly - the same API already proven working
// in firmware/main/wifi_setup.cpp's SoftAP setup form, so this side
// carries near-zero new risk. Unlike civetweb, esp_http_server's
// httpd_uri_t already supports per-method registration natively, so no
// internal method-dispatch layer is needed here (see HostHttpServer's
// header comment for why that side needs one).
class FirmwareHttpServer : public HttpServer {
public:
    FirmwareHttpServer() = default;
    ~FirmwareHttpServer() override;

    FirmwareHttpServer(const FirmwareHttpServer&) = delete;
    FirmwareHttpServer& operator=(const FirmwareHttpServer&) = delete;

    void RegisterHandler(HttpMethod method, const std::string& path, Handler handler) override;
    bool Start(uint16_t port) override;
    void Stop() override;

private:
    static esp_err_t DispatchTrampoline(httpd_req_t* req);

    httpd_handle_t server_ = nullptr;
    // Map values (not the map itself) must stay stable once Start() has
    // handed their addresses to esp_http_server as user_ctx - RegisterHandler()
    // must not be called again after Start(), per the base interface's
    // contract.
    std::map<std::pair<HttpMethod, std::string>, Handler> handlers_;
};

}  // namespace homedeck
