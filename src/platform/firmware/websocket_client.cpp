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
// in message_queue_ as if it were a real application message, corrupting
// the strict one-request/one-reply pairing HarmonyConnection's transport
// model depends on (see its own header comment).
// op_code never carries the FIN bit - tcp_transport's transport_ws.c parses
// it into a separate frame_state.fin bool and masks the opcode byte to its
// low 4 bits (frame_state.opcode = *data_ptr & 0x0F) before
// esp_transport_ws_get_read_opcode() ever returns it, so no masking is
// needed here.
bool IsApplicationDataOpcode(uint8_t op_code) {
    return op_code == WS_TRANSPORT_OPCODES_CONT || op_code == WS_TRANSPORT_OPCODES_TEXT ||
           op_code == WS_TRANSPORT_OPCODES_BINARY;
}

// Bounds how many complete messages can accumulate in message_queue_
// before the oldest is dropped - without a cap, any unsolicited message
// the hub sends outside the normal send-one/receive-one pattern (see
// core/harmony_connection.h's own comment on this transport's shape)
// would grow the queue without bound on a device that stays up for
// weeks, since nothing currently drains a message nobody asked for.
// core/harmony_connection.cpp's own DrainStaleMessages() loop
// (kMaxDrainIterations) is sized to match this value, so a full queue
// empties in one drain - keep the two in sync if this changes.
constexpr size_t kMaxQueuedMessages = 20;

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
    // HarmonyConnection/RetryBackoff owns reconnect policy - this
    // library's own auto-reconnect would otherwise retry independently
    // and race with that, per RetryBackoff's own comment.
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

    // A failure here means Connect() would otherwise block for the full
    // kConnectTimeoutMs with no way for it to ever succeed - no event
    // handler is registered to observe WEBSOCKET_EVENT_CONNECTED at all -
    // so this is reported immediately rather than left to degrade into a
    // silent timeout later.
    if (esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &OnWebSocketEvent, this) != ESP_OK) {
        ESP_LOGW(kTag, "esp_websocket_register_events() failed for %s", url.c_str());
        // Nothing left to retry against either way - client_ is abandoned
        // regardless of whether destroy() itself succeeds - but logging a
        // failure here leaves a trace if the underlying handle ever
        // actually leaks, instead of silently dropping it.
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
                                           [this] { return !message_queue_.empty() || closed_; });
    if (!got_message || message_queue_.empty()) {
        return std::nullopt;
    }
    std::string message = std::move(message_queue_.front());
    message_queue_.pop_front();
    return message;
}

void FirmwareWebSocketClient::Close() {
    if (client_ != nullptr) {
        // Failures here are logged, not otherwise acted upon - client_ is
        // abandoned either way (the object being torn down or reconnected
        // has nothing left to retry against) - but a log line leaves a
        // trace if the underlying handle is ever actually leaked, instead
        // of silently dropping it.
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
        message_queue_.clear();
        in_progress_message_.clear();
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
    // describe one chunk of a possibly-fragmented message, per ESP-IDF's
    // own websocket example. Exercised on-device against the reference
    // hub, not just the host backend - see roadmap.md's M3 Devices item,
    // whose 8-device config fetch (and the many-command-buttons LVGL bug
    // it surfaced) went through this exact path.
    const auto* data = static_cast<const esp_websocket_event_data_t*>(event_data);
    if (!IsApplicationDataOpcode(data->op_code)) {
        // PING/PONG/CLOSE - not a reply to anything HarmonyConnection
        // sent. The library already auto-PONGs a received PING and
        // reports a close via WEBSOCKET_EVENT_CLOSED/_DISCONNECTED
        // separately (see OnWebSocketEvent()); nothing to do here.
        return;
    }
    bool message_complete = false;
    bool oversized = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // See kMaxWebSocketMessageBytes's own comment (platform/
        // websocket_client.h) for why - this transport has no
        // authentication (ADR-0029). Checked before appending, not
        // after, so in_progress_message_ never actually exceeds the
        // bound even transiently.
        if (in_progress_message_.size() + static_cast<size_t>(data->data_len) > kMaxWebSocketMessageBytes) {
            in_progress_message_.clear();
            oversized = true;
        } else {
            in_progress_message_.append(static_cast<const char*>(data->data_ptr), data->data_len);
            if (data->payload_offset + data->data_len >= data->payload_len) {
                message_queue_.push_back(std::move(in_progress_message_));
                in_progress_message_.clear();
                // See kMaxQueuedMessages's own comment.
                while (message_queue_.size() > kMaxQueuedMessages) {
                    message_queue_.pop_front();
                }
                message_complete = true;
            }
        }
    }
    if (oversized) {
        // Same treatment as a transport-level close - ReceiveText()'s
        // caller (ConnectAndFetchConfig()/FetchCurrentActivity()) already
        // handles "connection dropped mid-receive" as a failed attempt,
        // which is the correct outcome here: an oversized message can't
        // be a well-formed hub response.
        HandleClosed();
        return;
    }
    if (message_complete) {
        queue_cv_.notify_one();
    }
}

}  // namespace homedeck
