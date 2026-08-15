#pragma once

#include "platform/websocket_client.h"

#include <condition_variable>
#include <deque>
#include <mutex>

// Forward-declared rather than including esp_websocket_client.h here, so
// this header stays includable from anything that only needs the
// WebSocketClient interface - same reasoning as HostWebSocketClient
// keeping CURL out of its own header.
typedef struct esp_websocket_client* esp_websocket_client_handle_t;

namespace homedeck {

// Backs WebSocketClient with espressif/esp_websocket_client. That
// library's native API is event/callback-based, driven by its own
// internal FreeRTOS task - this class bridges it to WebSocketClient's
// blocking shape with a small mutex/condition_variable-guarded queue,
// the same "adapt an async platform API to this project's blocking
// abstraction" shape platform/firmware already uses elsewhere (e.g.
// Rx8130TimeSource wrapping an interrupt-driven RTC). Auto-reconnect is
// disabled (see .cpp) - reconnection is HarmonyConnection/RetryBackoff's
// responsibility, not this transport's.
class FirmwareWebSocketClient : public WebSocketClient {
public:
    ~FirmwareWebSocketClient() override;

    bool Connect(const std::string& url) override;
    bool SendText(const std::string& text) override;
    std::optional<std::string> ReceiveText(int timeout_ms) override;
    void Close() override;

    // Called from the ESP-IDF event handler registered in Connect() (see
    // .cpp) - a free function there, not a static member, so this header
    // never needs esp_event_base_t/esp_event.h in scope. const void*
    // rather than esp_websocket_event_data_t* for the same reason. Public
    // only because that free function (not a friend - its signature lives
    // in the .cpp, matching esp_event_handler_t exactly, so it can't be
    // declared as one from here) needs to call these; not part of
    // WebSocketClient's own contract.
    void HandleConnected();
    void HandleClosed();
    void HandleData(const void* event_data);

private:
    esp_websocket_client_handle_t client_ = nullptr;

    std::mutex connect_mutex_;
    std::condition_variable connect_cv_;
    bool connect_pending_ = false;
    bool connect_succeeded_ = false;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> message_queue_;
    std::string in_progress_message_;
    bool closed_ = false;
};

}  // namespace homedeck
