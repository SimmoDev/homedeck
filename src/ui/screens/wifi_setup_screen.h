#pragma once

#include "core/event_bus.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "platform/network_status.h"
#include "ui/keyboard_input.h"
#include "ui/status_bar.h"

#include <functional>
#include <string>

namespace homedeck {

// Touch UI fallback for initial Wi-Fi setup - see
// docs/architecture/networking.md#initial-wi-fi-provisioning. The primary
// path is SoftAP plus a phone/laptop's browser form
// (firmware/main/wifi_setup.cpp); this exists for a user without a
// second device handy. Knows nothing about esp_wifi itself - submission
// goes through an injected callback, so this stays portable and
// simulator-testable like every other screen (see ADR-0004's UI
// state-management decision).
//
// Deliberately has no home affordance, unlike every other screen (see
// ADR-0004's "universal, every non-dashboard screen" home affordance
// decision) - going home before Wi-Fi is configured would strand the
// user on a screen with no network and no way back to this one short of
// a reboot, since nothing else ever navigates here automatically. See
// docs/architecture/ui.md#status for this documented exception.
class WifiSetupScreen {
public:
    using SubmitCallback = std::function<void(const std::string& ssid, const std::string& password)>;

    WifiSetupScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status,
                     SubmitCallback on_submit);
    // root_ has no owning parent (see ui.md#object-lifecycle) - deleting
    // it recursively deletes status_bar_/keyboard_'s LVGL objects too,
    // since both are its children.
    ~WifiSetupScreen();

    WifiSetupScreen(const WifiSetupScreen&) = delete;
    WifiSetupScreen& operator=(const WifiSetupScreen&) = delete;

    lv_obj_t* Root() const { return root_; }

    // Fills in the alternative-device setup instructions, only known once
    // SoftAP actually starts (see wifi_setup.cpp's WifiUiCallbacks) - the
    // screen exists before that, so this is a separate call rather than a
    // constructor parameter.
    void SetApInfo(const std::string& ap_ssid, const std::string& ap_ip);

    // Shows a connect-failure message (see WifiUiCallbacks::on_connect_failed) -
    // the only feedback this screen gives on a failed attempt, since
    // wifi_setup.cpp gives up silently otherwise. Cleared again as soon
    // as the user retries (see OnConnectClicked).
    void SetConnectError(const std::string& message);

private:
    static void OnConnectClicked(lv_event_t* e);

    lv_obj_t* root_;
    StatusBar status_bar_;
    OnScreenKeyboard keyboard_;
    lv_obj_t* ap_info_label_;
    lv_obj_t* ssid_field_;
    lv_obj_t* password_field_;
    lv_obj_t* error_label_;
    SubmitCallback on_submit_;
};

}  // namespace homedeck
