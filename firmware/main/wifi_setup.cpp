#include "wifi_setup.h"

#include "core/url_codec.h"
#include "core/wifi_credentials.h"
#include "core/wifi_reconnect_policy.h"

#include <cstdio>
#include <cstring>
#include <functional>
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

// Global mutable state - a deliberate exception, see
// docs/decisions/ADR-0026-wifi-provisioning-mechanism.md's own
// Consequences for why. Passed explicitly through
// esp_event_handler_register()'s event_handler_arg slot to OnEvent()
// below, rather than reached by name, so its dependency on this state
// is visible in the callback's own signature.
struct WifiSetupState {
    EventGroupHandle_t event_group = nullptr;
    httpd_handle_t setup_server = nullptr;
    // The reconnect-attempt-counting/give-up decision logic itself lives
    // in the portable, host-testable WifiReconnectPolicy
    // (src/core/wifi_reconnect_policy.h) - this file just executes
    // whatever it decides via the ESP-IDF APIs it can't be tested with.
    WifiReconnectPolicy reconnect_policy{kMaxSetupReconnectAttempts};
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
// comment), so pending_ssid/reconnect_policy genuinely have multiple
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
    // Worst case is a fully percent-encoded SSID (32 raw bytes -> up to
    // 96) plus password (64 raw bytes -> up to 192, WPA2's own max PSK
    // length), plus "ssid=" and "&password=" field-name overhead - a
    // smaller fixed buffer that just truncated whatever didn't fit
    // silently corrupted long/complex passwords instead of rejecting
    // them outright.
    constexpr size_t kMaxBodyBytes = 320;
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > kMaxBodyBytes) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body missing or too large");
        return ESP_FAIL;
    }
    // A single httpd_req_recv() call is not guaranteed to return the full
    // body - it can arrive split across TCP segments (e.g. a max-length
    // WPA2 password), silently truncating whatever didn't fit into one
    // read. Loop until the whole body is read, matching
    // FirmwareHttpServer::DispatchTrampoline's own fix for this bug
    // class (src/platform/firmware/http_server.cpp).
    constexpr int kMaxConsecutiveRecvTimeouts = 3;
    char body[kMaxBodyBytes + 1] = {};
    size_t total_received = 0;
    size_t body_len = static_cast<size_t>(req->content_len);
    int consecutive_timeouts = 0;
    while (total_received < body_len) {
        int received = httpd_req_recv(req, body + total_received, body_len - total_received);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++consecutive_timeouts > kMaxConsecutiveRecvTimeouts) {
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
            continue;
        }
        if (received <= 0) {
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        consecutive_timeouts = 0;
        total_received += static_cast<size_t>(received);
    }
    body[total_received] = '\0';

    // The setup page's <form> has no enctype, so browsers submit it as
    // application/x-www-form-urlencoded - spaces become '+' and symbols
    // become "%XX". ParseFormField decodes that (ESP-IDF's own
    // httpd_query_key_value(), used here previously, does not - it's a
    // raw byte copy, so any SSID/password containing a space or symbol
    // was applied to esp_wifi still percent-encoded and could never
    // associate).
    std::string ssid = ParseFormField(body, "ssid").value_or("");
    std::string password = ParseFormField(body, "password").value_or("");
    if (!ApplyWifiCredentials(ssid, password)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "Network name (SSID) is required, and both fields must be within Wi-Fi's length limits");
        return ESP_FAIL;
    }

    static const char kResponse[] =
        "<!DOCTYPE html><html><body style=\"font-family:sans-serif;max-width:400px;"
        "margin:40px auto;padding:0 16px\">"
        "<p>Connecting to Wi-Fi&hellip; you can close this page.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kResponse, HTTPD_RESP_USE_STRLEN);
}

void StartSetupHttpServer() {
    // Started into a local handle, not directly into g_state.setup_server -
    // httpd_start() can race OnEvent()'s lock-protected reads of that field
    // on another task otherwise (Wi-Fi events can fire as soon as
    // esp_wifi_start() returns, which happens before this is called - see
    // StartSetupAccessPoint() below). Published under g_state_mutex only
    // once the server is fully configured.
    httpd_handle_t server = nullptr;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    // Neither handler needs a user_ctx of its own - HandlePostConnect
    // reaches WifiSetupState only indirectly, through the public
    // ApplyWifiCredentials() below, the same entry point the Touch UI
    // fallback screen uses.
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = HandleGetSetupPage, .user_ctx = nullptr};
    httpd_uri_t connect_uri = {
        .uri = "/connect", .method = HTTP_POST, .handler = HandlePostConnect, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &connect_uri);

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.setup_server = server;
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
    // Copied under lock, then invoked outside it - consistent with this
    // file's own rule of never holding g_state_mutex across a call that
    // isn't a plain state read/write (see OnEvent()'s own comment above).
    std::function<void(const std::string&, const std::string&)> on_setup_needed;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        on_setup_needed = g_state.ui_callbacks.on_setup_needed;
    }
    if (on_setup_needed) {
        on_setup_needed(ap_ssid, kApGatewayIp);
    }
}

// arg is &g_state - registered twice below, for WIFI_EVENT and
// IP_EVENT, always with the same &g_state.
//
// Each branch below takes g_state_mutex only around the actual state
// reads/writes, never across httpd_stop()/esp_wifi_*() - both block (the
// former waits for the SoftAP httpd task to go idle, the latter is an RPC
// to the C6 co-processor, see hardware.md#wireless), and this function
// runs on the shared ESP-IDF event-loop task. HandlePostConnect()/
// ApplyWifiCredentials() (running on the httpd task) need the same
// mutex, so holding it across a blocking call here would let a second
// /connect submission landing mid-teardown deadlock the event-loop task
// against the httpd task.
void OnEvent(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto& state = *static_cast<WifiSetupState*>(arg);
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                // No WifiSetupState field is touched here - esp_wifi_connect()
                // stays outside g_state_mutex entirely, same reasoning as
                // ApplyWifiCredentials()'s own RPC calls below.
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                bool give_up = false;
                std::function<void()> on_connect_failed;
                {
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    if (state.network_status != nullptr) {
                        state.network_status->SetConnectionState(false, "", "");
                    }
                    give_up = state.reconnect_policy.OnDisconnected(state.setup_server != nullptr) ==
                              WifiReconnectPolicy::Decision::kGiveUp;
                    if (give_up) {
                        on_connect_failed = state.ui_callbacks.on_connect_failed;
                    }
                }
                // on_connect_failed() invoked outside g_state_mutex, same as
                // on_connected()/on_setup_needed() below - this function's
                // own invariant (see its comment above) is that a UI
                // callback never runs while holding the lock, since a
                // future implementation that blocks or re-enters
                // ApplyWifiCredentials()/ConnectToWifi() would otherwise
                // deadlock against the httpd task.
                if (give_up) {
                    ESP_LOGW(kTag, "Giving up after %d failed attempts - submit the setup form again to retry",
                             kMaxSetupReconnectAttempts);
                    if (on_connect_failed) {
                        on_connect_failed();
                    }
                    break;
                }
                ESP_LOGI(kTag, "Disconnected, retrying in %dms...", kReconnectBackoffMs);
                // Ignore the return - ESP_ERR_INVALID_STATE just means no
                // previous retry was pending, which is the common case.
                esp_timer_stop(g_reconnect_timer);
                ESP_ERROR_CHECK(esp_timer_start_once(g_reconnect_timer,
                                                      static_cast<uint64_t>(kReconnectBackoffMs) * 1000));
                break;
            }
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

        httpd_handle_t server_to_stop = nullptr;
        std::function<void()> on_connected;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            if (state.network_status != nullptr) {
                char ip_str[16];
                esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
                state.network_status->SetConnectionState(true, state.pending_ssid, ip_str);
            }
            server_to_stop = state.setup_server;
            state.setup_server = nullptr;
            on_connected = state.ui_callbacks.on_connected;
        }
        // Both block (see this function's own comment above) - deliberately
        // outside g_state_mutex.
        if (server_to_stop != nullptr) {
            httpd_stop(server_to_stop);
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        if (on_connected) {
            on_connected();
        }
        xEventGroupSetBits(state.event_group, kConnectedBit);
    }
}

}  // namespace

bool ApplyWifiCredentials(const std::string& ssid, const std::string& password) {
    if (!IsValidWifiCredentials(ssid, password)) {
        ESP_LOGW(kTag, "Rejecting Wi-Fi credentials submission (empty SSID or a field over its length limit)");
        return false;
    }
    wifi_config_t sta_config = {};
    std::snprintf(reinterpret_cast<char*>(sta_config.sta.ssid), sizeof(sta_config.sta.ssid), "%s", ssid.c_str());
    std::snprintf(reinterpret_cast<char*>(sta_config.sta.password), sizeof(sta_config.sta.password), "%s",
                  password.c_str());
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_state.pending_ssid = ssid;
        g_state.reconnect_policy.ResetAttempts();
    }
    ESP_LOGI(kTag, "Applying credentials, SSID: %s", sta_config.sta.ssid);
    // esp_wifi_set_config()/esp_wifi_connect() are RPC calls proxied to
    // the C6 co-processor over SDIO (see hardware.md#wireless for the
    // documented RPC-timeout risk) - kept outside g_state_mutex, same as
    // ConnectToWifi()'s equivalent esp_wifi_get_config() call below, so a
    // slow RPC here can't stall OnEvent() processing a concurrent Wi-Fi/
    // IP event on the shared ESP-IDF event-loop task.
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    // Connecting immediately below supersedes any retry a previous
    // disconnect already scheduled.
    esp_timer_stop(g_reconnect_timer);
    esp_wifi_connect();
    return true;
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

    EventGroupHandle_t event_group;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        event_group = g_state.event_group;
    }
    xEventGroupWaitBits(event_group, kConnectedBit, pdFALSE, pdTRUE, portMAX_DELAY);
}

}  // namespace homedeck
