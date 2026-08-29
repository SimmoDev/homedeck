#pragma once

#include <optional>
#include <string>

namespace homedeck {

// Every message either backend delivers to ReceiveText() (Harmony's hub
// config/current-activity responses, Kodi's JSON-RPC replies and pushed
// notifications - both connection-status/library-metadata scale, not
// media payloads) is at most a few hundred KB; 1 MiB is a generous
// multiple of that, not a tight fit. Both HostWebSocketClient and
// FirmwareWebSocketClient enforce this while accumulating a message
// across possibly-fragmented frames, since this transport has no
// authentication (ADR-0029, ADR-0030) - a rogue LAN device could
// otherwise send an arbitrarily large frame/message within the existing
// timeout, growing the accumulated string without bound.
constexpr size_t kMaxWebSocketMessageBytes = 1024 * 1024;

// Outbound WebSocket client - see docs/architecture/networking.md,
// ADR-0029, and ADR-0030. Text-frame (JSON) only - the only thing its
// two consumers (core/harmony_connection.h, core/kodi_client.h) need.
// Deliberately a blocking interface, not callback-based: callers own a
// dedicated Task (see platform/task.h) and drive a simple
// connect/send/receive loop on its own thread, the same shape
// OpenMeteoWeatherProvider's blocking HttpClient::Get() calls already
// use - so the two backends (libcurl on host, whose WS API is itself
// blocking-after-connect; esp_websocket_client on firmware, whose
// native API is event/callback-based and gets bridged to this blocking
// shape internally) both fit one interface without either forcing its
// own concurrency model on the other. No reconnect/backoff logic here -
// that's RetryBackoff's job, layered on top by the caller.
class WebSocketClient {
public:
    virtual ~WebSocketClient() = default;

    // Blocks until the WS handshake completes or fails. false on any
    // failure (DNS/connect/handshake) - callers can't distinguish which,
    // same success-only granularity HttpClient's transport-failure case
    // already has.
    virtual bool Connect(const std::string& url) = 0;

    // Sends one text frame. false if not connected or the send failed.
    virtual bool SendText(const std::string& text) = 0;

    // Blocks up to timeout_ms for the next complete text frame; returns
    // std::nullopt on timeout, connection close, or error - callers can't
    // distinguish those cases yet (nothing needs to). A bounded timeout,
    // not indefinite blocking, so a caller's Task can still notice
    // std::stop_token requests promptly (see Task's own destructor
    // contract).
    virtual std::optional<std::string> ReceiveText(int timeout_ms) = 0;

    virtual void Close() = 0;
};

}  // namespace homedeck
