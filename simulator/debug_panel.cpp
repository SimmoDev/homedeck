#include "debug_panel.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace homedeck::sim {

lv_obj_t* CreateTestButtonPanel(lv_obj_t* parent) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN_REVERSE);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 8, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

namespace {

void OnTestBackToDashboardClicked(lv_event_t* e) {
    auto* navigation = static_cast<Navigation*>(lv_event_get_user_data(e));
    navigation->GoHome();
}

}  // namespace

void CreateTestBackToDashboardButton(lv_obj_t* parent, Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    // Below the "To configure Wi-Fi..." instructions text and the
    // connect-error message beneath it (the last real content on this
    // screen, when SetConnectError has fired) with a visible gap - a
    // fixed offset rather than flex-flowed alongside it, since that text
    // is owned by the portable WifiSetupScreen (wifi_setup_screen.cpp)
    // and not exposed for a simulator-only debug button to attach to.
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 540);
    lv_obj_add_event_cb(button, OnTestBackToDashboardClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* label = lv_label_create(button);
    // WifiSetupScreen's root_ sets Montserrat 24 for its own real content
    // (see wifi_setup_screen.cpp), inherited here since this button is
    // parented to it - override back to LVGL's default size to match
    // every other Test: button, which are parented to
    // CreateTestButtonPanel()'s panel (no such override) and so stay at
    // the default.
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(label, "Test: back to dashboard");
}

namespace {

void OnTestWifiSetupNavClicked(lv_event_t* e) {
    auto* navigation = static_cast<Navigation*>(lv_event_get_user_data(e));
    navigation->GoTo("wifi-setup");
}

}  // namespace

void CreateTestWifiSetupNavButton(lv_obj_t* parent, Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestWifiSetupNavClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: go to Wi-Fi setup screen");
}

namespace {

void OnTestWifiDisconnectClicked(lv_event_t* e) {
    auto* network_status = static_cast<HostNetworkStatus*>(lv_event_get_user_data(e));
    network_status->SetConnected(!network_status->Snapshot().connected);
}

}  // namespace

void CreateTestWifiDisconnectButton(lv_obj_t* parent, HostNetworkStatus& network_status) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestWifiDisconnectClicked, LV_EVENT_CLICKED, &network_status);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle wifi disconnected");
}

namespace {

// Runs on UiTask's own thread (the one already running
// lv_timer_handler()), so this freezes the simulator window for the
// clip's ~0.3s duration - a known, accepted tradeoff for a manual test
// button, not worth a Task-based async wrapper (see
// platform/audio_output.h's own blocking contract).
void OnTestPlayToneClicked(lv_event_t* e) {
    auto* audio_output = static_cast<HostAudioOutput*>(lv_event_get_user_data(e));
    constexpr uint32_t kSampleRate = 48000;
    constexpr double kDurationSeconds = 0.3;
    constexpr double kToneHz = 440.0;
    std::vector<int16_t> tone(static_cast<size_t>(kSampleRate * kDurationSeconds));
    for (size_t i = 0; i < tone.size(); ++i) {
        double t = static_cast<double>(i) / kSampleRate;
        tone[i] = static_cast<int16_t>(std::sin(2 * M_PI * kToneHz * t) * 10000);
    }
    audio_output->SetVolume(70);
    audio_output->Play(tone.data(), tone.size(), kSampleRate);
}

}  // namespace

void CreateTestPlayToneButton(lv_obj_t* parent, HostAudioOutput& audio_output) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestPlayToneClicked, LV_EVENT_CLICKED, &audio_output);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: play tone");
}

namespace {

void OnTestLowBatteryClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetPercent(10);
}

}  // namespace

void CreateTestLowBatteryButton(lv_obj_t* parent, HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestLowBatteryClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger low battery");
}

namespace {

void OnTestCriticalBatteryClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetPercent(2);
}

}  // namespace

void CreateTestCriticalBatteryButton(lv_obj_t* parent, HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestCriticalBatteryClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger critical battery");
}

namespace {

void OnTestExternalPowerClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetExternalPowerConnected(!battery_reader->IsExternalPowerConnected());
}

}  // namespace

void CreateTestExternalPowerButton(lv_obj_t* parent, HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestExternalPowerClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle external power");
}

namespace {

void OnTestBatteryPresentClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetBatteryPresent(!battery_reader->IsBatteryPresent());
}

}  // namespace

void CreateTestBatteryPresentButton(lv_obj_t* parent, HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestBatteryPresentClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle battery present");
}

namespace {

void OnTestForceOtaFailureClicked(lv_event_t* e) {
    auto* force_failure = static_cast<bool*>(lv_event_get_user_data(e));
    *force_failure = !*force_failure;
}

}  // namespace

void CreateTestForceOtaFailureButton(lv_obj_t* parent, bool& force_failure) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestForceOtaFailureClicked, LV_EVENT_CLICKED, &force_failure);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle force OTA failure");
}

namespace {

void OnTestTriggerIdleClicked(lv_event_t* e) {
    auto* source = static_cast<DebugOverridableUserActivitySource*>(lv_event_get_user_data(e));
    source->SetForced(DebugOverridableUserActivitySource::ForcedLevel::kIdle);
}

}  // namespace

void CreateTestTriggerIdleButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestTriggerIdleClicked, LV_EVENT_CLICKED, &source);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger idle");
}

namespace {

void OnTestTriggerSleepingClicked(lv_event_t* e) {
    auto* source = static_cast<DebugOverridableUserActivitySource*>(lv_event_get_user_data(e));
    source->SetForced(DebugOverridableUserActivitySource::ForcedLevel::kSleeping);
}

}  // namespace

void CreateTestTriggerSleepingButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestTriggerSleepingClicked, LV_EVENT_CLICKED, &source);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger sleeping");
}

namespace {

void OnTestTriggerActiveClicked(lv_event_t* e) {
    auto* source = static_cast<DebugOverridableUserActivitySource*>(lv_event_get_user_data(e));
    source->SetForced(DebugOverridableUserActivitySource::ForcedLevel::kNone);
    // Clears the override, but the underlying real clock needs to be
    // fresh too - otherwise it could still read as idle on the very next
    // tick if the real SDL window hasn't actually been touched recently.
    lv_display_trigger_activity(nullptr);
}

}  // namespace

void CreateTestTriggerActiveButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestTriggerActiveClicked, LV_EVENT_CLICKED, &source);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger active");
}

namespace {

void OnTestLogEntryClicked(lv_event_t* e) {
    auto* logger = static_cast<Logger*>(lv_event_get_user_data(e));
    logger->Log(LogLevel::kInfo, "simulator", "Test log entry");
}

}  // namespace

void CreateTestLogEntryButton(lv_obj_t* parent, Logger& logger) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestLogEntryClicked, LV_EVENT_CLICKED, &logger);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: write a log entry");
}

}  // namespace homedeck::sim
