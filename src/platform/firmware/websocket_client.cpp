#include "platform/firmware/websocket_client.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "websocket_client";
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 10000;

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
    closed_ = false;

    // A failure here means Connect() would otherwise block for the full
    // kConnectTimeoutMs with no way for it to ever succeed - no event
    // handler is registered to observe WEBSOCKET_EVENT_CONNECTED at all -
    // so this is reported immediately rather than left to degrade into a
    // silent timeout later.
    if (esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &OnWebSocketEvent, this) != ESP_OK) {
        ESP_LOGW(kTag, "esp_websocket_register_events() failed for %s", url.c_str());
        esp_websocket_client_destroy(client_);
        client_ = nullptr;
        return false;
    }

    if (esp_websocket_client_start(client_) != ESP_OK) {
        esp_websocket_client_destroy(client_);
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
        esp_websocket_client_stop(client_);
        esp_websocket_client_destroy(client_);
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
    bool message_complete = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        in_progress_message_.append(static_cast<const char*>(data->data_ptr), data->data_len);
        if (data->payload_offset + data->data_len >= data->payload_len) {
            message_queue_.push_back(std::move(in_progress_message_));
            in_progress_message_.clear();
            message_complete = true;
        }
    }
    if (message_complete) {
        queue_cv_.notify_one();
    }
}

}  // namespace homedeck
