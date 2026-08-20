#pragma once

#include "platform/websocket_client.h"

namespace homedeck {

// Backs WebSocketClient with libcurl for the raw connection only (reuses
// the library already linked for HostHttpClient,
// platform/host/http_client.h, rather than adding a new host-only
// dependency) plus hand-rolled RFC 6455 handshake and framing for
// everything after - not curl's own curl_ws_send()/curl_ws_recv() API,
// despite it existing in the linked library, and not curl's own HTTP
// engine for the handshake either. Confirmed on a stock Ubuntu 24.04
// system libcurl (8.5.0): curl_version_info() reports the WS *feature*
// bit and the curl_ws_* symbols are present, but "ws"/"wss" are absent
// from its own registered protocol list - CURLOPT_URL rejects a ws://
// URL outright with CURLE_UNSUPPORTED_PROTOCOL, and curl_ws_send/
// curl_ws_recv are gated on that same protocol registration internally,
// so they're unusable regardless of the symbols being linkable. This
// isn't a version-pin problem (7.86 was ruled out as the bar) - it's a
// packaging choice orthogonal to version.
//
// A tempting-looking workaround - CURLOPT_CONNECT_ONLY=2 against a plain
// http:// URL with the Upgrade handshake headers added manually
// (CURLOPT_HTTPHEADER) - does NOT work: curl's generic HTTP engine has
// no way to know a 101 response is bodyless without going through the
// ws:// scheme handler this build lacks, so curl_easy_perform() hangs
// waiting for a body or connection close that a real WS server -
// correctly keeping the connection open for the ensuing frame exchange -
// never sends. Confirmed working instead: CURLOPT_CONNECT_ONLY=1 (the
// original, protocol-generic form - stop right after the TCP/TLS
// connect, before curl sends any request of its own), then the Upgrade
// request/response and all WS framing are hand-built over
// curl_easy_send()/curl_easy_recv() - curl's own documented use case for
// CONNECT_ONLY, and TLS-transparent for a future wss:// caller (unlike
// raw socket I/O on the extracted fd, which would read/write encrypted
// bytes as if they were plaintext).
//
// CURL's own type is kept out of this header (a void* instead) so
// callers of this header never need <curl/curl.h> in scope, the same
// header/impl split HostHttpClient itself uses.
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
