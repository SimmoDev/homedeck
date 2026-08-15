#pragma once

#include "platform/websocket_client.h"

namespace homedeck {

// Backs WebSocketClient with libcurl's WS API (curl_ws_recv/curl_ws_send,
// available since curl 7.86) - reuses the library already linked for
// HostHttpClient (platform/host/http_client.h) rather than adding a new
// host-only dependency. CURL's own type is kept out of this header (a
// void* instead) so callers of this header never need <curl/curl.h> in
// scope, the same header/impl split HostHttpClient itself uses.
class HostWebSocketClient : public WebSocketClient {
public:
    ~HostWebSocketClient() override;

    bool Connect(const std::string& url) override;
    bool SendText(const std::string& text) override;
    std::optional<std::string> ReceiveText(int timeout_ms) override;
    void Close() override;

private:
    void* curl_ = nullptr;  // CURL*, see websocket_client.cpp
};

}  // namespace homedeck
