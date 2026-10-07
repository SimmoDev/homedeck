#include "platform/firmware/websocket_client.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_transport_ws.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "websocket_client";
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 10000;

// esp_websocket_client dispatches every received WebSocket frame through
// WEBSOCKET_EVENT_DATA unconditionally (esp_websocket_client_recv() calls
// esp_websocket_client_dispatch_event(..., WEBSOCKET_EVENT_DATA, ...)
// before its own opcode-specific handling - auto-PONGing a PING, clearing
// wait_for_pong_resp on a PONG, entering WEBSOCKET_STATE_CLOSING on a
// CLOSE - runs). Connect() below leaves ping_interval_sec at the
// library's own 10s default, so a PING/PONG keepalive round-trip happens
// on this schedule for as long as the connection stays open. Left
// unfiltered, a PONG's empty-payload WEBSOCKET_EVENT_DATA would be queued
// in message_queue_ as if it were an application message, corrupting
// the request/reply pairing every consumer relies on.
// op_code never carries the FIN bit - tcp_transport's transport_ws.c parses
// it into a separate frame_state.fin bool and masks the opcode byte to its
// low 4 bits (frame_state.opcode = *data_ptr & 0x0F) before
// esp_transport_ws_get_read_opcode() ever returns it, so no masking is
// needed here.
bool IsApplicationDataOpcode(uint8_t op_code) {
    return op_code == WS_TRANSPORT_OPCODES_CONT || op_code == WS_TRANSPORT_OPCODES_TEXT ||
           op_code == WS_TRANSPORT_OPCODES_BINARY;
}

// The ESP-IDF event-loop callback esp_websocket_register_events() wants -
// a free function matching esp_event_handler_t exactly, not a static
// class member, so websocket_client.h never needs esp_event_base_t in
// scope (see its own comment). handler_args is the FirmwareWebSocketClient
// passed to esp_websocket_register_events() below.
void OnWebSocketEvent(void* handler_args, esp_event_base_t /*base*/, int32_t event_id, void* event_data) {
    auto* self = static_cast<FirmwareWebSocketClient*>(handler_args);
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            self->HandleConnected();
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_ERROR:
        case WEBSOCKET_EVENT_CLOSED:
            self->HandleClosed();
            break;
        case WEBSOCKET_EVENT_DATA:
            self->HandleData(event_data);
            break;
        default:
            break;
    }
}

}  // namespace

FirmwareWebSocketClient::~FirmwareWebSocketClient() { Close(); }

bool FirmwareWebSocketClient::Connect(const std::string& url) {
    Close();

    esp_websocket_client_config_t config = {};
    config.uri = url.c_str();
    // The module's RetryBackoff owns reconnect policy; the library's own
    // auto-reconnect would retry independently and race with it.
    config.disable_auto_reconnect = true;

    client_ = esp_websocket_client_init(&config);
    if (client_ == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        connect_pending_ = true;
        connect_succeeded_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        closed_ = false;
    }

    // Without the handler nothing observes WEBSOCKET_EVENT_CONNECTED, so
    // Connect() would wait out kConnectTimeoutMs and fail anyway; report it now.
    if (esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &OnWebSocketEvent, this) != ESP_OK) {
        ESP_LOGW(kTag, "esp_websocket_register_events() failed for %s", url.c_str());
        // client_ is abandoned whether or not destroy() succeeds; the log
        // line marks a possible handle leak.
        if (esp_websocket_client_destroy(client_) != ESP_OK) {
            ESP_LOGW(kTag, "esp_websocket_client_destroy() failed after a failed register_events()");
        }
        client_ = nullptr;
        return false;
    }

    if (esp_websocket_client_start(client_) != ESP_OK) {
        if (esp_websocket_client_destroy(client_) != ESP_OK) {
            ESP_LOGW(kTag, "esp_websocket_client_destroy() failed after a failed start()");
        }
        client_ = nullptr;
        return false;
    }

    // esp_websocket_client_start() itself is asynchronous - the actual
    // TCP connect + WS handshake happens on the library's own task,
    // reported back via WEBSOCKET_EVENT_CONNECTED/_ERROR - so this
    // blocks (bounded) until one of those arrives, to give Connect() the
    // same synchronous contract HostWebSocketClient's curl backend has
    // naturally.
    std::unique_lock<std::mutex> lock(connect_mutex_);
    bool signaled = connect_cv_.wait_for(lock, std::chrono::milliseconds(kConnectTimeoutMs),
                                          [this] { return !connect_pending_; });
    if (!signaled || !connect_succeeded_) {
        lock.unlock();
        Close();
        return false;
    }
    return true;
}

bool FirmwareWebSocketClient::SendText(const std::string& text) {
    if (client_ == nullptr) {
        return false;
    }
    int sent = esp_websocket_client_send_text(client_, text.data(), static_cast<int>(text.size()),
                                                pdMS_TO_TICKS(kSendTimeoutMs));
    return sent == static_cast<int>(text.size());
}

std::optional<std::string> FirmwareWebSocketClient::ReceiveText(int timeout_ms) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    bool got_message = queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                           [this] { return assembler_.HasMessage() || closed_; });
    if (!got_message || !assembler_.HasMessage()) {
        return std::nullopt;
    }
    return assembler_.Pop();
}

bool FirmwareWebSocketClient::IsOpen() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return client_ != nullptr && !closed_;
}

void FirmwareWebSocketClient::Close() {
    if (client_ != nullptr) {
        // client_ is abandoned whether or not stop()/destroy() succeed; the
        // log lines mark a possible handle leak.
        if (esp_websocket_client_stop(client_) != ESP_OK) {
            ESP_LOGW(kTag, "esp_websocket_client_stop() failed");
        }
        if (esp_websocket_client_destroy(client_) != ESP_OK) {
            ESP_LOGW(kTag, "esp_websocket_client_destroy() failed - the underlying handle may be leaked");
        }
        client_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        assembler_.Clear();
        closed_ = true;
    }
    queue_cv_.notify_all();
}

void FirmwareWebSocketClient::HandleConnected() {
    {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        connect_pending_ = false;
        connect_succeeded_ = true;
    }
    connect_cv_.notify_one();
}

void FirmwareWebSocketClient::HandleClosed() {
    {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        if (connect_pending_) {
            connect_pending_ = false;
            connect_succeeded_ = false;
        }
    }
    connect_cv_.notify_one();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        closed_ = true;
    }
    queue_cv_.notify_all();
}

void FirmwareWebSocketClient::HandleData(const void* event_data) {
    // esp_websocket_event_data_t - payload_offset/payload_len/data_len
    // describe one chunk of a frame, per ESP-IDF's own websocket example;
    // `fin` marks the last frame of a message that may span several
    // frames (a CONT opcode follows a frame with fin unset).
    const auto* data = static_cast<const esp_websocket_event_data_t*>(event_data);
    if (!IsApplicationDataOpcode(data->op_code)) {
        // PING/PONG/CLOSE - not a reply to anything a consumer
        // sent. The library already auto-PONGs a received PING and
        // reports a close via WEBSOCKET_EVENT_CLOSED/_DISCONNECTED
        // separately (see OnWebSocketEvent()); nothing to do here.
        return;
    }
    WebSocketMessageAssembler::Result result;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // Once closed (including by the oversize path below) the rest of the
        // message in flight is discarded rather than queued as a fragment.
        if (closed_) {
            return;
        }
        result = assembler_.Add(static_cast<const char*>(data->data_ptr), static_cast<size_t>(data->data_len), data->fin,
                                data->payload_offset + data->data_len >= data->payload_len);
    }
    if (result == WebSocketMessageAssembler::Result::kOversized) {
        // Treated as a transport close: an oversized message can't be a
        // well-formed response, and callers already handle a dropped link.
        HandleClosed();
        return;
    }
    if (result == WebSocketMessageAssembler::Result::kComplete) {
        queue_cv_.notify_one();
    }
}

}  // namespace homedeck
