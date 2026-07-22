#include "platform/firmware/http_client.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "http_client";

// Bounds how long Task::~Task() can block on an in-flight fetch - see
// core/weather_provider.h's own comment on why an unbounded timeout
// would undercut the chunked-sleep "prompt shutdown" contract that
// exists specifically so a Task built on this doesn't hang on
// destruction.
constexpr int kTimeoutMs = 10000;

// esp_http_client_perform() discards the response body unless captured
// here - there's no "just give me the body" option in the simple API.
// event->data/data_len is always the already-dechunked payload for this
// event regardless of Transfer-Encoding - esp_http_client_is_chunked_
// response() is for callers choosing whether to trust Content-Length
// for a fixed-size buffer, not a signal to skip appending here (Open-
// Meteo's own response is chunked, confirmed on hardware).
esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        auto* body = static_cast<std::string*>(event->user_data);
        body->append(static_cast<char*>(event->data), event->data_len);
    }
    return ESP_OK;
}

}  // namespace

HttpClientResponse FirmwareHttpClient::Get(const std::string& url) {
    std::string body;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = kTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = OnHttpEvent;
    config.user_data = &body;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    HttpClientResponse response;
    response.success = (err == ESP_OK);
    response.status_code = response.success ? esp_http_client_get_status_code(client) : 0;
    response.body = std::move(body);

    if (!response.success) {
        ESP_LOGW(kTag, "GET %s failed: %s", url.c_str(), esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return response;
}

}  // namespace homedeck
