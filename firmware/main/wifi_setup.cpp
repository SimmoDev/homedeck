#include "wifi_setup.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "bsp/m5stack_tab5.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

namespace homedeck {

namespace {

constexpr char kTag[] = "wifi_setup";
constexpr int kConnectedBit = BIT0;

// Only enforced while WifiSetupState::setup_server is running (freshly-
// submitted, maybe wrong, credentials) - not during normal reconnects to
// a network the device already trusts, where giving up would strand it
// with no Wi-Fi and no way back into setup mode. Resubmitting the setup
// form resets the count, so a mistyped password is always recoverable.
constexpr int kMaxSetupReconnectAttempts = 5;
// Fixed, not exponential - this is a single always-on-battery-or-mains
// device reconnecting to one specific already-trusted AP, not a fleet of
// clients that could pile up and overwhelm a recovering service (the
// scenario docs/architecture/networking.md's module-level retry/backoff
// contract exists for). A short fixed interval recovers from a router
// reboot quickly without meaningfully affecting battery life.
constexpr int kReconnectBackoffMs = 500;

// esp_netif's default AP gateway - not currently derived from
// esp_netif_get_ip_info() since the default AP netif config is never
// overridden here.
constexpr char kApGatewayIp[] = "192.168.4.1";

// Single-instance by hardware necessity (one Wi-Fi radio), not a
// convenience shortcut. Passed explicitly through
// esp_event_handler_register()'s event_handler_arg slot to OnEvent()
// below, rather than reached by name, so its dependency on this state
// is visible in the callback's own signature.
struct WifiSetupState {
    EventGroupHandle_t event_group = nullptr;
    httpd_handle_t setup_server = nullptr;
    int reconnect_attempts = 0;
    WifiUiCallbacks ui_callbacks;
    FirmwareNetworkStatus* network_status = nullptr;
    // The SSID we're configured/attempting to connect to - captured from
    // existing esp_wifi_get_config()/ApplyWifiCredentials() call sites
    // (zero extra cost) rather than a fresh esp_wifi_sta_get_ap_info()
    // call inside OnEvent(), which would be a new esp_wifi_remote RPC
    // round trip to the C6 on every connect (see hardware.md's Wireless
    // section for the documented RPC-timeout risk this avoids). Not
    // cleared on disconnect - it still describes what we're configured
    // for, useful for the next reconnect attempt.
    std::string pending_ssid;
};

// Guards every WifiSetupState field below that's touched after boot -
// OnEvent() runs on the ESP event-loop task, while ApplyWifiCredentials()/
// ConnectToWifi() are called from either the SoftAP HTTP form's own worker
// task, the Touch UI's LVGL task, or app startup (see wifi_setup.h's own
// comment), so pending_ssid/reconnect_attempts genuinely have multiple
// unsynchronized writers/readers without this. Never held across a wait -
// see g_reconnect_timer below for why the reconnect delay isn't a
// vTaskDelay() inside this lock.
std::mutex g_state_mutex;
WifiSetupState g_state;

// Schedules a delayed esp_wifi_connect() retry without blocking the
// caller. OnEvent() runs on the shared ESP-IDF default event-loop task
// while holding g_state_mutex - sleeping there (a prior version of this
// file used vTaskDelay()) would stall every other event on that loop and
// every other g_state_mutex waiter for the full retry interval, for as
// long as the outage lasts. esp_timer's own callback runs on a dedicated
// task, and esp_timer_start_once()/esp_timer_stop() are safe to call from
// any task, so scheduling one is a fast, non-blocking operation. Created
// once in InitWifiAndCheckStoredCredentials(), before Wi-Fi can generate
// its first disconnect event.
esp_timer_handle_t g_reconnect_timer = nullptr;

void ReconnectTimerCallback(void* /*arg*/) { esp_wifi_connect(); }

void GetApSsid(char* ssid, size_t max_len) {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    std::snprintf(ssid, max_len, "HomeDeck-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

esp_err_t HandleGetSetupPage(httpd_req_t* req) {
    static const char kPage[] =
        "<!DOCTYPE html><html><head>"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>HomeDeck Wi-Fi setup</title></head>"
        "<body style=\"font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 16px\">"
        "<h1>HomeDeck Wi-Fi setup</h1>"
        "<form method=\"POST\" action=\"/connect\">"
        "<label>Network name (SSID)<br>"
        "<input name=\"ssid\" required style=\"width:100%\"></label><br><br>"
        "<label>Password<br>"
        "<input name=\"password\" type=\"password\" style=\"width:100%\"></label><br><br>"
        "<button type=\"submit\">Connect</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t HandlePostConnect(httpd_req_t* req) {
    char body[192] = {};
    size_t to_read = std::min(static_cast<size_t>(req->content_len), sizeof(body) - 1);
    int received = httpd_req_recv(req, body, to_read);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {};
    char password[65] = {};
    httpd_query_key_value(body, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(body, "password", password, sizeof(password));
    ApplyWifiCredentials(ssid, password);

    static const char kResponse[] =
        "<!DOCTYPE html><html><body style=\"font-family:sans-serif;max-width:400px;"
        "margin:40px auto;padding:0 16px\">"
        "<p>Connecting to Wi-Fi&hellip; you can close this page.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kResponse, HTTPD_RESP_USE_STRLEN);
}

void StartSetupHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&g_state.setup_server, &config));

    // Neither handler needs a user_ctx of its own - HandlePostConnect
    // reaches WifiSetupState only indirectly, through the public
    // ApplyWifiCredentials() below, the same entry point the Touch UI
    // fallback screen uses.
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = HandleGetSetupPage, .user_ctx = nullptr};
    httpd_uri_t connect_uri = {
        .uri = "/connect", .method = HTTP_POST, .handler = HandlePostConnect, .user_ctx = nullptr};
    httpd_register_uri_handler(g_state.setup_server, &root_uri);
    httpd_register_uri_handler(g_state.setup_server, &connect_uri);
}

void StartSetupAccessPoint() {
    char ap_ssid[24];
    GetApSsid(ap_ssid, sizeof(ap_ssid));

    wifi_config_t ap_config = {};
    std::snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ap_ssid);
    ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ap_ssid));
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(kTag, "Not provisioned - connect to Wi-Fi network '%s' (open) and visit "
                    "http://%s/ to set up HomeDeck",
             ap_ssid, kApGatewayIp);

    StartSetupHttpServer();
    if (g_state.ui_callbacks.on_setup_needed) {
        g_state.ui_callbacks.on_setup_needed(ap_ssid, kApGatewayIp);
    }
}

// arg is &g_state - registered twice below, for WIFI_EVENT and
// IP_EVENT, always with the same &g_state.
void OnEvent(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    auto& state = *static_cast<WifiSetupState*>(arg);
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if (state.network_status != nullptr) {
                    state.network_status->SetConnectionState(false, "", "");
                }
                if (state.setup_server != nullptr && state.reconnect_attempts >= kMaxSetupReconnectAttempts) {
                    ESP_LOGW(kTag, "Giving up after %d failed attempts - submit the setup form again to retry",
                             kMaxSetupReconnectAttempts);
                    break;
                }
                ++state.reconnect_attempts;
                ESP_LOGI(kTag, "Disconnected, retrying in %dms...", kReconnectBackoffMs);
                // Ignore the return - ESP_ERR_INVALID_STATE just means no
                // previous retry was pending, which is the common case.
                esp_timer_stop(g_reconnect_timer);
                ESP_ERROR_CHECK(esp_timer_start_once(g_reconnect_timer,
                                                      static_cast<uint64_t>(kReconnectBackoffMs) * 1000));
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        // A retry scheduled just before this connection succeeded would
        // otherwise still fire later and call esp_wifi_connect() again
        // while already connected - harmless, but pointless.
        esp_timer_stop(g_reconnect_timer);
        ESP_LOGI(kTag, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (state.network_status != nullptr) {
            char ip_str[16];
            esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
            state.network_status->SetConnectionState(true, state.pending_ssid, ip_str);
        }
        if (state.setup_server != nullptr) {
            httpd_stop(state.setup_server);
            state.setup_server = nullptr;
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        if (state.ui_callbacks.on_connected) {
            state.ui_callbacks.on_connected();
        }
        xEventGroupSetBits(state.event_group, kConnectedBit);
    }
}

}  // namespace

void ApplyWifiCredentials(const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.pending_ssid = ssid;
    wifi_config_t sta_config = {};
    std::snprintf(reinterpret_cast<char*>(sta_config.sta.ssid), sizeof(sta_config.sta.ssid), "%s", ssid.c_str());
    std::snprintf(reinterpret_cast<char*>(sta_config.sta.password), sizeof(sta_config.sta.password), "%s",
                  password.c_str());
    ESP_LOGI(kTag, "Applying credentials, SSID: %s", sta_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    g_state.reconnect_attempts = 0;
    // Connecting immediately below supersedes any retry a previous
    // disconnect already scheduled.
    esp_timer_stop(g_reconnect_timer);
    esp_wifi_connect();
}

WifiCredentialsCheck InitWifiAndCheckStoredCredentials(FirmwareNetworkStatus& network_status) {
    // Set before registering the event handlers below, not after - see
    // this function's own declaration comment in wifi_setup.h for why
    // the ordering matters.
    g_state.network_status = &network_status;

    // Powers on the C6 co-processor's rail via the PI4IOE5V6408 I2C GPIO
    // expander - see docs/architecture/hardware.md#wireless. Without this
    // the SDIO link to the C6 never comes up.
    bsp_feature_enable(BSP_FEATURE_WIFI, true);

    g_state.event_group = xEventGroupCreate();

    esp_timer_create_args_t reconnect_timer_args = {};
    reconnect_timer_args.callback = &ReconnectTimerCallback;
    reconnect_timer_args.name = "wifi_reconnect";
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &g_reconnect_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnEvent, &g_state));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnEvent, &g_state));

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));

    wifi_config_t sta_config = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sta_config));
    g_state.pending_ssid = reinterpret_cast<const char*>(sta_config.sta.ssid);

    char ap_ssid[24];
    GetApSsid(ap_ssid, sizeof(ap_ssid));

    return WifiCredentialsCheck{sta_config.sta.ssid[0] != '\0', ap_ssid, kApGatewayIp};
}

void ConnectToWifi(const WifiUiCallbacks& ui_callbacks) {
    wifi_config_t sta_config = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sta_config));
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_state.ui_callbacks = ui_callbacks;
        g_state.pending_ssid = reinterpret_cast<const char*>(sta_config.sta.ssid);
    }

    if (sta_config.sta.ssid[0] != '\0') {
        ESP_LOGI(kTag, "Stored credentials found for '%s', connecting", sta_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        StartSetupAccessPoint();
    }

    xEventGroupWaitBits(g_state.event_group, kConnectedBit, pdFALSE, pdTRUE, portMAX_DELAY);
}

}  // namespace homedeck
