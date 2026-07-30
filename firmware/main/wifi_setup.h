#pragma once

#include "platform/firmware/network_status.h"

#include <functional>
#include <string>

namespace homedeck {

// wifi_setup.cpp has no LVGL/Navigation dependency of its own - these are
// how it reaches the Touch UI, dispatched onto the UI task by whoever
// provides them (see ConnectToWifi's own comment for why that dispatch
// can't happen inside this file).
struct WifiUiCallbacks {
    // Fires once, only when no stored credentials exist and SoftAP setup
    // is starting - the caller should show the Touch UI fallback screen,
    // passing ap_ssid/ap_ip through to it (see WifiSetupScreen::SetApInfo)
    // so it can tell the user what to connect to as an alternative. May
    // be empty (no Touch UI hookup).
    std::function<void(const std::string& ap_ssid, const std::string& ap_ip)> on_setup_needed;
    // Fires once Wi-Fi actually connects - the caller should dismiss the
    // setup screen. May be empty.
    std::function<void()> on_connected;
    // Fires when a freshly-submitted set of credentials gives up after
    // kMaxSetupReconnectAttempts failed attempts (wrong password, AP out
    // of range, etc.) - the SoftAP and setup HTTP server both stay up
    // regardless, so resubmitting the form is always still possible; this
    // just tells a caller that already showed the Touch UI fallback
    // screen to surface that the last attempt didn't work. Never fires
    // once already connected (see OnEvent's own comment for why normal
    // post-setup reconnects don't have this cap). May be empty.
    std::function<void()> on_connect_failed;
};

// The outcome of checking stored credentials, without yet attempting to
// connect - see InitWifiAndCheckStoredCredentials() below. ap_ssid/ap_ip
// are always populated (derived from the device's own MAC and the fixed
// SoftAP gateway address respectively), regardless of
// has_stored_credentials, since a caller deciding what to show the user
// needs them before knowing whether setup mode will actually be entered.
struct WifiCredentialsCheck {
    bool has_stored_credentials;
    std::string ap_ssid;
    std::string ap_ip;
};

// Brings up the Wi-Fi subsystem (SDIO/ESP-Hosted link to the C6 - see
// docs/architecture/hardware.md#wireless) far enough to answer "are
// credentials already stored," without attempting to associate or
// starting SoftAP yet. Split out from ConnectToWifi() below so a caller
// that wants to pick the correct initial Touch UI screen (dashboard vs.
// the Wi-Fi setup fallback) can do so before ever showing a screen,
// rather than always showing the dashboard first and correcting course
// once Wi-Fi state becomes known.
//
// Must be called after esp_netif_init() and
// esp_event_loop_create_default(), and before ConnectToWifi() below.
//
// Also wires up `network_status` here rather than in ConnectToWifi()
// below, specifically because the WIFI_EVENT/IP_EVENT handlers that
// write into it are registered inside this function - wiring it in any
// later would leave a real window where a live event (e.g. from the
// Touch UI Wi-Fi setup screen submitting credentials directly) could
// fire before network_status is set. Must outlive every subsequent
// call in this file.
WifiCredentialsCheck InitWifiAndCheckStoredCredentials(FirmwareNetworkStatus& network_status);

// Continues from InitWifiAndCheckStoredCredentials() above. If credentials
// are already stored, connects directly; otherwise starts a temporary
// open SoftAP ("HomeDeck-xxxxxx") and a minimal HTTP setup form at its
// gateway address, and blocks until a phone or laptop submits Wi-Fi
// credentials through it (or the Touch UI fallback screen submits them
// via ApplyWifiCredentials below) and the device connects. See
// docs/architecture/networking.md#initial-wi-fi-provisioning and ADR-0006
// for why this is a small HomeDeck-owned HTTP form rather than ESP-IDF's
// wifi_provisioning component (a real incompatibility with this
// project's esp_wifi_remote stack, not a design preference).
void ConnectToWifi(const WifiUiCallbacks& ui_callbacks = {});

// Applies newly-submitted credentials and attempts to connect - shared by
// the SoftAP HTTP form's own handler and the Touch UI fallback screen, so
// neither reimplements esp_wifi_set_config/connect and the reconnect-
// attempt reset independently. Returns false without touching esp_wifi at
// all if ssid is empty or either field exceeds esp_wifi's own length
// limits (see core/wifi_credentials.h) - both callers surface that back
// to the user (a 400 response for the HTTP form,
// WifiSetupScreen::SetConnectError for the Touch UI) rather than
// silently attempting to associate with a blank SSID or letting the
// snprintf() below truncate an overlong field.
bool ApplyWifiCredentials(const std::string& ssid, const std::string& password);

}  // namespace homedeck
