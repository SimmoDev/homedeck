#include "wifi_setup.h"

#include "core/url_codec.h"
#include "core/wifi_credentials.h"
#include "core/wifi_reconnect_policy.h"

#include <cassert>
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

// Only enforced during the device's very first, no-stored-credentials
// setup flow (WifiSetupState::initial_provisioning) - not during normal
// reconnects to a network the device already trusts, where giving up
// would strand it with no Wi-Fi and no way back into setup mode.
// Resubmitting the setup form resets the count, so a mistyped password
// is always recoverable.
constexpr int kMaxSetupReconnectAttempts = 5;
// A normal-mode (already-provisioned) reconnect never gives up per the
// above, but silently retrying forever with no way for the user to
// intervene is its own real gap if the stored network is genuinely gone
// for good (moved house, router replaced) rather than just briefly down
// - see WifiReconnectPolicy::ShouldOfferRecovery() and
// StartRecoveryAccessPoint() below. Each failed attempt costs ~2.9s
// end to end (kReconnectBackoffMs's 500ms plus the SDIO round trip to
// the C6 for esp_wifi_connect() to actually fail),
// not the bare 500ms backoff alone - 40 attempts is ~2 minutes at that
// real rate, long enough to ride out a router reboot without offering a
// recovery access point prematurely, short enough that a genuinely-gone
// network doesn't leave the device silently unreachable indefinitely.
constexpr int kNormalModeRecoveryAttempts = 40;
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

// The initial no-stored-credentials setup flow's HTTP server runs before
// homedeck.cpp's own admin Web UI (`web_server`) ever starts (see
// ConnectToWifi()'s call sequence in app_main()), so the default HTTP
// port/ctrl port are always free for it.
constexpr uint16_t kSetupHttpPort = 80;
// StartRecoveryAccessPoint() (below) can only ever run *after* a first
// successful connect already started `web_server` on kSetupHttpPort/
// ESP_HTTPD_DEF_CTRL_PORT (see FinalizeBootAfterWifiConnected() in
// homedeck.cpp, which never stops it again) - binding the recovery
// server to either of those would fail (esp_http_server's SO_REUSEADDR
// only permits rebinding a port already in TIME_WAIT, not two
// simultaneously live listeners) and abort the firmware via this file's
// own ESP_ERROR_CHECK-on-httpd_start precedent, crashing the device in
// exactly the outage this access point exists to recover from. A
// distinct port and ctrl port sidesteps the conflict entirely.
constexpr uint16_t kRecoveryHttpPort = 8080;
constexpr uint16_t kRecoveryHttpCtrlPort = ESP_HTTPD_DEF_CTRL_PORT + 1;
// HTTPD_DEFAULT_CONFIG()'s max_open_sockets=7 default costs
// max_open_sockets+3 real LWIP sockets per httpd instance
// (esp_http_server's own httpd_main.c) - 10 out of this project's
// CONFIG_LWIP_MAX_SOCKETS=10 budget for homedeck.cpp's `web_server`
// alone, already running by the time this server can start (see
// kRecoveryHttpPort's own comment). Left at the default, this server's
// accept() calls fail outright once web_server has any active
// connections ("httpd_accept_conn: error in accept (23)" server-side, a
// dropped connection client-side). This server only ever needs to serve
// one client's setup form at a time, so a small fixed value both avoids
// that and leaves real headroom in the shared budget (see
// sdkconfig.defaults' CONFIG_LWIP_MAX_SOCKETS override for the other
// half of this fix).
constexpr size_t kSetupHttpMaxOpenSockets = 3;

// "HomeDeck-" (9 chars) + 6 hex digits of MAC + null terminator, shared
// by every GetApSsid() caller below (StartSetupAccessPoint(),
// InitWifiAndCheckStoredCredentials(), StartRecoveryAccessPoint()) rather
// than each sizing its own local buffer independently.
constexpr size_t kApSsidBufferSize = 24;

// Global mutable state - a deliberate exception, see
// docs/decisions/ADR-0026-wifi-provisioning-mechanism.md's own
// Consequences for why. Passed explicitly through
// esp_event_handler_register()'s event_handler_arg slot to OnEvent()
// below, rather than reached by name, so its dependency on this state
// is visible in the callback's own signature.
struct WifiSetupState {
    EventGroupHandle_t event_group = nullptr;
    httpd_handle_t setup_server = nullptr;
    // The reconnect-attempt-counting/give-up/offer-recovery decision
    // logic itself lives in the portable, host-testable
    // WifiReconnectPolicy (src/core/wifi_reconnect_policy.h) - this file
    // just executes whatever it decides via the ESP-IDF APIs it can't be
    // tested with.
    WifiReconnectPolicy reconnect_policy{kMaxSetupReconnectAttempts, kNormalModeRecoveryAttempts};
    // True only during the device's very first, no-stored-credentials
    // setup flow (set in ConnectToWifi() before StartSetupAccessPoint(),
    // cleared on a successful connect) - distinct from `setup_server !=
    // nullptr`, which also becomes true once StartRecoveryAccessPoint()
    // brings the *same* access point/form up as a later recovery path.
    // Kept separate so a long-running, already-provisioned device that
    // falls back to recovery doesn't inherit the initial flow's much
    // smaller give-up cap (WifiReconnectPolicy::OnDisconnected() would
    // otherwise stop retrying almost immediately once setup_server
    // became non-null for the recovery reason instead).
    bool initial_provisioning = false;
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

void ReconnectTimerCallback(void* /*arg*/) {
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        // Same underlying situation WIFI_EVENT_STA_START's own handler
        // documents (a failed esp_wifi_connect() means no
        // WIFI_EVENT_STA_DISCONNECTED fires for this attempt either), but
        // higher-stakes here: this timer is what the retry chain re-arms
        // itself with on every disconnect for as long as the device runs,
        // not a one-time boot-path call - leaving it unhandled would let a
        // single transient failure permanently stall reconnection with no
        // log line and no further attempts. Reschedules directly rather
        // than routing through OnEvent()'s WIFI_EVENT_STA_DISCONNECTED
        // bookkeeping (reconnect_policy's give-up/recovery attempt
        // counting) - this call never actually started a connection to
        // fail, so it isn't the kind of attempt that counting tracks.
        ESP_LOGW(kTag, "esp_wifi_connect (on reconnect timer) failed: %s - rescheduling in %dms", esp_err_to_name(err),
                 kReconnectBackoffMs);
        esp_timer_stop(g_reconnect_timer);
        esp_err_t start_err =
            esp_timer_start_once(g_reconnect_timer, static_cast<uint64_t>(kReconnectBackoffMs) * 1000);
        if (start_err != ESP_OK) {
            ESP_LOGE(kTag, "Failed to reschedule the reconnect timer: %s", esp_err_to_name(start_err));
        }
    }
}

// Runs StartRecoveryAccessPoint() (defined below, forward-declared here)
// off the ESP-IDF event-loop task - OnEvent() itself must never block
// (see its own comment), and bringing up an access point involves
// several blocking esp_wifi_*()/httpd_start() RPC calls, the same reason
// g_reconnect_timer exists for the plain reconnect retry. A second,
// separate timer rather than reusing g_reconnect_timer, since both need
// to be independently schedulable - a recovery access point coming up
// must not cancel or delay the plain STA reconnect retry loop still
// running alongside it.
esp_timer_handle_t g_recovery_timer = nullptr;
void StartRecoveryAccessPoint();
void RecoveryTimerCallback(void* /*arg*/) { StartRecoveryAccessPoint(); }

void GetApSsid(char* ssid, size_t max_len) {
    // Zero-initialized, not left uninitialized: this SSID is the recovery
    // hotspot's name, the only thing a locked-out user has to find it by -
    // a failed read must still produce a deterministic, loggable name, not
    // whatever garbage was on the stack.
    uint8_t mac[6] = {};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_get_mac failed - recovery AP SSID will use a zeroed suffix");
    }
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
    // kMaxConsecutiveRecvTimeouts alone bounds silence, not total transfer
    // time - a client trickling in a single byte just under every timeout
    // would reset that counter forever and hold this server's one worker
    // thread indefinitely, blocking the setup form for anyone else -
    // matching http_server.cpp's own kMaxTotalRecvTimeouts fix for the
    // identical bug class, same reasoning, same value even though this
    // body is far smaller (consistency over precision - the bound is a
    // safety ceiling, not something a legitimate 320-byte form submission
    // ever approaches).
    constexpr int kMaxTotalRecvTimeouts = 120;
    char body[kMaxBodyBytes + 1] = {};
    size_t total_received = 0;
    size_t body_len = static_cast<size_t>(req->content_len);
    int consecutive_timeouts = 0;
    int total_timeouts = 0;
    while (total_received < body_len) {
        int received = httpd_req_recv(req, body + total_received, body_len - total_received);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++consecutive_timeouts > kMaxConsecutiveRecvTimeouts || ++total_timeouts > kMaxTotalRecvTimeouts) {
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
    // become "%XX". ParseFormField decodes that; ESP-IDF's own
    // httpd_query_key_value() does not - it's a raw byte copy, so an
    // SSID/password containing a space or symbol would reach esp_wifi
    // still percent-encoded and could never associate.
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

// port/ctrl_port let the two callers below bind to different sockets -
// see kRecoveryHttpPort's own comment for why that matters. Returns
// whatever httpd_start() returned rather than aborting itself, so
// StartRecoveryAccessPoint() can degrade gracefully on failure the same
// way every other call in that function already does; StartSetupAccessPoint()
// below still escalates a failure to a hard abort via ESP_ERROR_CHECK,
// since it has nothing else to fall back to this early in boot.
esp_err_t StartSetupHttpServer(uint16_t port, uint16_t ctrl_port) {
    // Started into a local handle, not directly into g_state.setup_server -
    // httpd_start() can race OnEvent()'s lock-protected reads of that field
    // on another task otherwise (Wi-Fi events can fire as soon as
    // esp_wifi_start() returns, which happens before this is called - see
    // StartSetupAccessPoint() below). Published under g_state_mutex only
    // once the server is fully configured.
    httpd_handle_t server = nullptr;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = ctrl_port;
    config.max_open_sockets = kSetupHttpMaxOpenSockets;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        return err;
    }

    // Neither handler needs a user_ctx of its own - HandlePostConnect
    // reaches WifiSetupState only indirectly, through the public
    // ApplyWifiCredentials() below, the same entry point the Touch UI
    // fallback screen uses.
    httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = HandleGetSetupPage, .user_ctx = nullptr};
    httpd_uri_t connect_uri = {
        .uri = "/connect", .method = HTTP_POST, .handler = HandlePostConnect, .user_ctx = nullptr};
    if (httpd_register_uri_handler(server, &root_uri) != ESP_OK) {
        ESP_LOGW(kTag, "Failed to register the setup page's '/' route");
    }
    if (httpd_register_uri_handler(server, &connect_uri) != ESP_OK) {
        ESP_LOGW(kTag, "Failed to register the setup page's '/connect' route");
    }

    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state.setup_server = server;
    return ESP_OK;
}

void StartSetupAccessPoint() {
    char ap_ssid[kApSsidBufferSize];
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

    // Nothing else in this no-stored-credentials boot path can proceed
    // without a working setup server - matches every esp_wifi_*() call
    // above, which already treats failure as unrecoverable via
    // ESP_ERROR_CHECK.
    ESP_ERROR_CHECK(StartSetupHttpServer(kSetupHttpPort, ESP_HTTPD_DEF_CTRL_PORT));
    // Copied under lock, then invoked outside it - consistent with this
    // file's own rule of never holding g_state_mutex across a call that
    // isn't a plain state read/write (see OnEvent()'s own comment above).
    std::function<void(const std::string&, const std::string&, uint16_t)> on_setup_needed;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        on_setup_needed = g_state.ui_callbacks.on_setup_needed;
    }
    if (on_setup_needed) {
        on_setup_needed(ap_ssid, kApGatewayIp, kSetupHttpPort);
    }
}

// Brings up the same open SoftAP + minimal HTTP setup form as
// StartSetupAccessPoint() above, but as a recovery path *after* Wi-Fi was
// already configured and once connected - see
// WifiReconnectPolicy::ShouldOfferRecovery()'s own comment for exactly
// when this fires (a long, unbroken run of normal-mode reconnect
// failures). Without this, a device whose stored credentials stop
// working after working once (moved house, router replaced) retries
// forever with no user-reachable way back in: the Web UI never starts
// (firmware/main/homedeck.cpp blocks in ConnectToWifi() until connected)
// if this is the very first connect attempt of the boot, and the Touch UI
// has no affordance to re-enter WifiSetupScreen outside the
// no-stored-credentials path. Runs on kRecoveryHttpPort, not
// kSetupHttpPort - unlike that first-boot case, this can *also* fire
// after a long *mid-session* outage (Wi-Fi worked for a while, then a
// long-lived router/AP failure crosses the recovery threshold), by which
// point homedeck.cpp's own admin Web UI has already started and is still
// listening on kSetupHttpPort/ESP_HTTPD_DEF_CTRL_PORT for the rest of the
// process's life (it's never stopped) - binding this server to either
// would fail outright (two live listeners can't share one port).
//
// Unlike StartSetupAccessPoint(), Wi-Fi is already running in STA mode
// here (mid reconnect-retry loop) - esp_wifi_start() can't be called
// again without first stopping the driver (ESP-IDF returns
// ESP_ERR_WIFI_STATE for a second start while already started).
// Stop-then-reconfigure-then-start is the conservative, well-established
// sequence for adding AP mode to an already-running STA session,
// regardless of whether a live mode switch without stopping first would
// also work - not verified against real Tab5 hardware,
// unlike the rest of this file's Wi-Fi behavior (see hardware.md's own
// "Confirmed" vs. not-yet-confirmed convention for why that distinction
// matters here). Every esp_wifi_*() call below is checked explicitly
// rather than via ESP_ERROR_CHECK for the same reason: this path only
// runs after the device already connected once, so an unexpected failure
// here should degrade to "still retrying STA in the background, no
// recovery access point this attempt" rather than crash a device that
// was otherwise working - the plain reconnect timer keeps running
// regardless of whether this succeeds.
void StartRecoveryAccessPoint() {
    char ap_ssid[kApSsidBufferSize];
    GetApSsid(ap_ssid, sizeof(ap_ssid));

    if (esp_wifi_stop() != ESP_OK) {
        ESP_LOGW(kTag, "StartRecoveryAccessPoint: esp_wifi_stop failed, skipping this attempt");
        return;
    }

    wifi_config_t ap_config = {};
    std::snprintf(reinterpret_cast<char*>(ap_config.ap.ssid), sizeof(ap_config.ap.ssid), "%s", ap_ssid);
    ap_config.ap.ssid_len = static_cast<uint8_t>(std::strlen(ap_ssid));
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 4;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGW(kTag, "StartRecoveryAccessPoint: failed to bring up the recovery access point");
        return;
    }

    // kRecoveryHttpPort's failure path here degrades the same way every
    // other call in this function already does - the AP itself is already
    // up (Wi-Fi-adjacent recovery still works, e.g. a Touch UI resubmission
    // if it's ever reachable another way), just without this particular
    // HTTP form; the plain reconnect timer keeps running regardless.
    if (StartSetupHttpServer(kRecoveryHttpPort, kRecoveryHttpCtrlPort) != ESP_OK) {
        ESP_LOGW(kTag, "StartRecoveryAccessPoint: failed to start the recovery HTTP server");
        return;
    }

    ESP_LOGW(kTag,
             "Still disconnected after %d attempts - offering recovery Wi-Fi network '%s' (open) at http://%s:%u/ "
             "alongside continued reconnect attempts",
             kNormalModeRecoveryAttempts, ap_ssid, kApGatewayIp, kRecoveryHttpPort);

    // Same publish-then-invoke pattern as StartSetupAccessPoint() above -
    // see its own comment.
    std::function<void(const std::string&, const std::string&, uint16_t)> on_setup_needed;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        on_setup_needed = g_state.ui_callbacks.on_setup_needed;
    }
    if (on_setup_needed) {
        on_setup_needed(ap_ssid, kApGatewayIp, kRecoveryHttpPort);
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
            case WIFI_EVENT_STA_START: {
                // No WifiSetupState field is touched here - esp_wifi_connect()
                // stays outside g_state_mutex entirely, same reasoning as
                // ApplyWifiCredentials()'s own RPC calls below.
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    // A failure here means WIFI_EVENT_STA_DISCONNECTED
                    // never fires either (the connection never started),
                    // so the retry logic below would otherwise never
                    // engage - logged so a stuck-unconnected device isn't
                    // silently unexplained.
                    ESP_LOGW(kTag, "esp_wifi_connect (on STA start) failed: %s", esp_err_to_name(err));
                }
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                bool give_up = false;
                bool offer_recovery = false;
                std::function<void()> on_connect_failed;
                {
                    std::lock_guard<std::mutex> lock(g_state_mutex);
                    if (state.network_status != nullptr) {
                        state.network_status->SetConnectionState(false, "", "");
                    }
                    give_up = state.reconnect_policy.OnDisconnected(state.initial_provisioning) ==
                              WifiReconnectPolicy::Decision::kGiveUp;
                    if (give_up) {
                        on_connect_failed = state.ui_callbacks.on_connect_failed;
                    } else if (state.setup_server == nullptr) {
                        // Only offer recovery if some access point/form
                        // isn't already up - ShouldOfferRecovery() itself
                        // only returns true once per accrued-attempts
                        // threshold, but this also skips it while the
                        // initial no-stored-credentials flow already has
                        // its own access point up (initial_provisioning
                        // is true there, so ShouldOfferRecovery() would
                        // already return false, but checking setup_server
                        // too keeps this branch's intent explicit).
                        offer_recovery = state.reconnect_policy.ShouldOfferRecovery(state.initial_provisioning);
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
                if (offer_recovery) {
                    // Scheduled, not called directly - see
                    // StartRecoveryAccessPoint()'s own comment for why
                    // this can't run on this task. The plain reconnect
                    // retry below is still scheduled regardless -
                    // bringing up recovery never pauses it.
                    if (esp_timer_start_once(g_recovery_timer, 0) != ESP_OK) {
                        ESP_LOGW(kTag, "Failed to schedule the recovery access point - staying unreachable");
                    }
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
            state.initial_provisioning = false;
            // A real successful connection deserves its own full set of
            // attempts before either the setup-mode cap or the recovery
            // threshold applies again, the same as a fresh credential
            // submission already gets via ApplyWifiCredentials() - see
            // WifiReconnectPolicy::ResetAttempts()'s own comment. Without
            // this, a device that reconnects successfully and later loses
            // Wi-Fi again for a second, separate long outage would never
            // be offered recovery again for the rest of its uptime.
            state.reconnect_policy.ResetAttempts();
            on_connected = state.ui_callbacks.on_connected;
        }
        // Both block (see this function's own comment above) - deliberately
        // outside g_state_mutex.
        if (server_to_stop != nullptr) {
            esp_err_t stop_err = httpd_stop(server_to_stop);
            if (stop_err != ESP_OK) {
                ESP_LOGW(kTag, "Failed to stop the recovery access point's httpd: %s", esp_err_to_name(stop_err));
            }
            if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
                ESP_LOGW(kTag, "Failed to drop out of AP+STA mode after recovery - staying in AP+STA");
            }
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
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_connect (on credential submission) failed: %s", esp_err_to_name(err));
    }
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

    esp_timer_create_args_t recovery_timer_args = {};
    recovery_timer_args.callback = &RecoveryTimerCallback;
    recovery_timer_args.name = "wifi_recovery_ap";
    ESP_ERROR_CHECK(esp_timer_create(&recovery_timer_args, &g_recovery_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &OnEvent, &g_state));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnEvent, &g_state));

    // Every call below this point that touches WIFI_IF_STA/WIFI_IF_AP
    // assumes both netifs exist - a null here is an unrecoverable boot-time
    // misconfiguration (e.g. out of memory this early), not a case with a
    // meaningful fallback, so it's fatal the same way the ESP_ERROR_CHECK()
    // calls below already are.
    assert(esp_netif_create_default_wifi_sta() != nullptr);
    assert(esp_netif_create_default_wifi_ap() != nullptr);

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_config));

    wifi_config_t sta_config = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sta_config));
    g_state.pending_ssid = reinterpret_cast<const char*>(sta_config.sta.ssid);

    char ap_ssid[kApSsidBufferSize];
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
        // Set before StartSetupAccessPoint() (not held across it - see
        // OnEvent()'s own comment on why blocking esp_wifi_*() calls stay
        // outside g_state_mutex) so the very first disconnect this flow
        // can generate already sees initial_provisioning as true.
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_state.initial_provisioning = true;
        }
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
