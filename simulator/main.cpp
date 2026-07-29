#include "core/admin_auth_service.h"
#include "core/clock.h"
#include "core/critical_battery_monitor.h"
#include "core/diagnostics_routes.h"
#include "core/event_bus.h"
#include "core/logger.h"
#include "core/low_battery_monitor.h"
#include "core/network_status_monitor.h"
#include "core/notification_sound.h"
#include "core/ota_routes.h"
#include "core/power_manager.h"
#include "core/settings_routes.h"
#include "core/storage.h"
#include "core/weather_provider.h"
#include "core/weather_routes.h"
#include "platform/host/audio_output.h"
#include "platform/host/battery_reader.h"
#include "platform/host/cache_store.h"
#include "platform/host/display_brightness.h"
#include "platform/host/file_backed_store.h"
#include "platform/host/http_client.h"
#include "platform/host/http_server.h"
#include "platform/host/network_status.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/time_source.h"
#include "platform/host/ui_task.h"
#include "platform/static_assets.h"
#include "platform/steady_time_source.h"
#include "ui/clock_widget.h"
#include "ui/lvgl_user_activity_source.h"
#include "ui/navigation.h"
#include "ui/network_status_widget.h"
#include "ui/notification_banner.h"
#include "ui/notification_widget.h"
#include "ui/quick_settings_panel.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/screens/wifi_setup_screen.h"
#include "ui/status_bar.h"
#include "ui/weather_widget.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <tuple>
#include <vector>

// Matches the Tab5's native panel resolution and orientation - portrait,
// not the 1280x720 spec-sheet landscape figure (see
// docs/architecture/hardware.md#display-driver-strategy for why).
static constexpr int32_t kWindowWidth = 720;
static constexpr int32_t kWindowHeight = 1280;

// Desktop dev-convenience only: scales the on-screen window so a
// 720x1280 logical canvas doesn't demand 1280px of vertical monitor
// space. LVGL still renders at the real logical resolution above -
// layout behaves identically to hardware, just displayed smaller. Any
// zoom other than 1.0 softens text somewhat, even with UiTask's
// scale-quality hint (see src/ui/ui_task.cpp) - real hardware is
// unaffected, since it never scales at all. No single value fits every
// desktop/taskbar layout, so override at runtime with HOMEDECK_SIM_ZOOM
// (e.g. `HOMEDECK_SIM_ZOOM=0.6 ./homedeck_simulator`) rather than editing
// this default.
static constexpr float kDefaultWindowZoom = 0.75f;

// The Web Management UI's default port. Real hardware uses 80 (see
// firmware/main/homedeck.cpp); a desktop dev machine typically can't
// bind that without root, so the simulator defaults elsewhere, with the
// same override convention as HOMEDECK_SIM_ZOOM above.
static constexpr uint16_t kDefaultWebPort = 8080;

namespace {

// Shared shape with firmware/main/homedeck.cpp's identical helper -
// duplicated rather than factored into Core, same reasoning as
// notification_sound.cpp's own tone generator: small enough that a
// shared abstraction for two call sites isn't worth it. std::from_chars,
// not std::stoi - matches Storage's own internal parsing (see
// storage.cpp's Decode()) for the same firmware-builds-without-
// exceptions reason, even though only the simulator strictly needs it
// here - keeping both call sites identical is worth more than exploiting
// a target-specific shortcut.
int ReadIntSetting(homedeck::Storage& storage, const char* module_id, const char* key, int fallback) {
    std::optional<homedeck::VersionedValue> setting = storage.GetSetting(module_id, key);
    if (!setting) {
        return fallback;
    }
    int value = 0;
    auto [ptr, ec] = std::from_chars(setting->value.data(), setting->value.data() + setting->value.size(), value);
    if (ec != std::errc{} || ptr != setting->value.data() + setting->value.size()) {
        return fallback;
    }
    return value;
}

float ResolveWindowZoom() {
    const char* override_str = std::getenv("HOMEDECK_SIM_ZOOM");
    if (override_str == nullptr) {
        return kDefaultWindowZoom;
    }
    char* end = nullptr;
    float value = std::strtof(override_str, &end);
    if (end == override_str || value <= 0.0f) {
        return kDefaultWindowZoom;
    }
    return value;
}

uint16_t ResolveWebPort() {
    const char* override_str = std::getenv("HOMEDECK_SIM_WEB_PORT");
    if (override_str == nullptr) {
        return kDefaultWebPort;
    }
    char* end = nullptr;
    long value = std::strtol(override_str, &end, 10);
    if (end == override_str || value <= 0 || value > 65535) {
        return kDefaultWebPort;
    }
    return static_cast<uint16_t>(value);
}

// A bottom-anchored flex column all the CreateTestXButton() helpers
// below attach to, so a new debug button just gets added to the flow -
// no manually-chosen pixel offset to pick, and no need to renumber
// other buttons' offsets if one is removed. COLUMN_REVERSE so each new
// call appends above the previous one, closest-to-edge first. Cross
// axis (horizontal, for a column flow) must be CENTER, not END - END
// right-aligns each button against the panel's own content-sized width
// instead of centering it.
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

// Temporary test-only wiring proving LowBatteryMonitor/NotificationBanner
// end to end - HostBatteryReader is a fixed-then-adjustable mock (see
// platform/host/battery_reader.h) that never naturally crosses the low-
// battery threshold on its own, so there's no other way to see the real
// notification flow run in the simulator. Removed once a real widget
// (weather) or some other real trigger exists to exercise this
// naturally; kept out of LowBatteryMonitor itself so real product code
// stays free of throwaway test scaffolding, the same reasoning
// CreateTestNavButton above already follows.
void OnTestLowBatteryClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetPercent(10);
}

void CreateTestLowBatteryButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestLowBatteryClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger low battery");
}

// Same reasoning as CreateTestLowBatteryButton above, for
// CriticalBatteryMonitor/PowerManager's kError transition instead - 2%
// is below CriticalBatteryMonitor::kCriticalThresholdPercent (5), the
// same margin style this button's 10 uses against LowBatteryMonitor's
// 15.
void OnTestCriticalBatteryClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetPercent(2);
}

void CreateTestCriticalBatteryButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestCriticalBatteryClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger critical battery");
}

// Temporary test-only wiring proving GET /api/diagnostics' external-power
// field end to end - HostBatteryReader's external-power flag never
// changes on its own, the same reasoning CreateTestLowBatteryButton
// above already follows. Removed once a real Power Management screen
// exists to exercise this.
void OnTestExternalPowerClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetExternalPowerConnected(!battery_reader->IsExternalPowerConnected());
}

void CreateTestExternalPowerButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestExternalPowerClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle external power");
}

// Temporary test-only wiring proving LowBatteryMonitor doesn't fire
// (and GET /api/diagnostics' batteryPresent field reflects reality)
// when no battery is installed - the same reasoning
// CreateTestExternalPowerButton above already follows. Removed once a
// real Power Management screen exists to exercise this.
void OnTestBatteryPresentClicked(lv_event_t* e) {
    auto* battery_reader = static_cast<homedeck::HostBatteryReader*>(lv_event_get_user_data(e));
    battery_reader->SetBatteryPresent(!battery_reader->IsBatteryPresent());
}

void CreateTestBatteryPresentButton(lv_obj_t* parent, homedeck::HostBatteryReader& battery_reader) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestBatteryPresentClicked, LV_EVENT_CLICKED, &battery_reader);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle battery present");
}

// Temporary test-only wiring proving the Web UI's OTA page surfaces a
// real upload failure, not just the success path - see
// docs/architecture/simulator.md's OTA mock description. Removed once a
// real Power Management screen (or similar) exists to exercise this.
void OnTestForceOtaFailureClicked(lv_event_t* e) {
    auto* force_failure = static_cast<bool*>(lv_event_get_user_data(e));
    *force_failure = !*force_failure;
}

void CreateTestForceOtaFailureButton(lv_obj_t* parent, bool& force_failure) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestForceOtaFailureClicked, LV_EVENT_CLICKED, &force_failure);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle force OTA failure");
}

// Temporary test-only wiring proving the Web UI's new Logs section
// renders real entries without needing multiple simulator restarts to
// accumulate them - see docs/decisions/ADR-0019-structured-logging.md.
// Removed once a real, naturally-occurring event exists to exercise
// this in the simulator (there's no Wi-Fi/mDNS bring-up here to log,
// unlike firmware).
void OnTestLogEntryClicked(lv_event_t* e) {
    auto* logger = static_cast<homedeck::Logger*>(lv_event_get_user_data(e));
    logger->Log(homedeck::LogLevel::kInfo, "simulator", "Test log entry");
}

void CreateTestLogEntryButton(lv_obj_t* parent, homedeck::Logger& logger) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestLogEntryClicked, LV_EVENT_CLICKED, &logger);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: write a log entry");
}

// Temporary test-only wiring to reach the Wi-Fi setup screen - the
// simulator has no real SoftAP "not provisioned" path to trigger it
// naturally (see wifi_setup_screen.h), the same reasoning
// CreateTestLowBatteryButton below already follows for LowBatteryMonitor.
// Removed once a real trigger exists.
void OnTestWifiSetupNavClicked(lv_event_t* e) {
    auto* navigation = static_cast<homedeck::Navigation*>(lv_event_get_user_data(e));
    navigation->GoTo("wifi-setup");
}

void CreateTestWifiSetupNavButton(lv_obj_t* parent, homedeck::Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestWifiSetupNavClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: go to Wi-Fi setup screen");
}

// WifiSetupScreen deliberately omits the home affordance (see ui.md's
// Navigation model) - correct on real hardware, since an unprovisioned
// device shouldn't let the user bail back to a dashboard with no
// network, but it leaves the simulator with no way back to the
// dashboard once CreateTestWifiSetupNavButton above navigates here.
// Parented to wifi_setup_screen.Root() itself, not dashboard.Root() -
// this screen's controls are the only ones visible once it's loaded.
// Removed once a real trigger exists, same as the button above. Placed
// just below the screen's own content rather than bottom-anchored like
// every other Test: button - this screen's on-screen keyboard occupies
// the bottom 40% of the screen once a text field is focused (see
// keyboard_input.cpp), and a bottom-anchored button would sit on top of
// (and block) part of it.
void OnTestBackToDashboardClicked(lv_event_t* e) {
    auto* navigation = static_cast<homedeck::Navigation*>(lv_event_get_user_data(e));
    navigation->GoHome();
}

void CreateTestBackToDashboardButton(lv_obj_t* parent, homedeck::Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    // Below the "To configure Wi-Fi..." instructions text (the last
    // real content on this screen) with a visible gap - a fixed offset
    // rather than flex-flowed alongside it, since that text is owned by
    // the portable WifiSetupScreen (wifi_setup_screen.cpp) and not
    // exposed for a simulator-only debug button to attach to.
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 460);
    lv_obj_add_event_cb(button, OnTestBackToDashboardClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* label = lv_label_create(button);
    // WifiSetupScreen's root_ sets Montserrat 24 for its own real
    // content (see wifi_setup_screen.cpp), inherited here since this
    // button is parented to it - override back to LVGL's default size
    // to match every other Test: button, which are parented to
    // CreateTestButtonPanel()'s panel (no such override) and so stay at
    // the default.
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_label_set_text(label, "Test: back to dashboard");
}

// Temporary test-only wiring to exercise StatusBar's and
// NetworkStatusWidget's disconnected rendering - HostNetworkStatus
// defaults to connected with nothing to disconnect it, the same
// reasoning CreateTestLowBatteryButton above already follows. Removed
// once a real trigger exists.
void OnTestWifiDisconnectClicked(lv_event_t* e) {
    auto* network_status = static_cast<homedeck::HostNetworkStatus*>(lv_event_get_user_data(e));
    network_status->SetConnected(!network_status->Snapshot().connected);
}

void CreateTestWifiDisconnectButton(lv_obj_t* parent, homedeck::HostNetworkStatus& network_status) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestWifiDisconnectClicked, LV_EVENT_CLICKED, &network_status);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: toggle wifi disconnected");
}

// Not temporary - there's no other way to manually exercise
// AudioOutput in the simulator, the same lasting-dev-hook role
// CreateTestLowBatteryButton above already plays for its own hardware
// signal. Runs on UiTask's own thread (the one already running
// lv_timer_handler()), so this freezes the simulator window for the
// clip's ~0.3s duration - a known, accepted tradeoff for a manual test
// button, not worth a Task-based async wrapper (see
// platform/audio_output.h's own blocking contract).
void OnTestPlayToneClicked(lv_event_t* e) {
    auto* audio_output = static_cast<homedeck::HostAudioOutput*>(lv_event_get_user_data(e));
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

void CreateTestPlayToneButton(lv_obj_t* parent, homedeck::HostAudioOutput& audio_output) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestPlayToneClicked, LV_EVENT_CLICKED, &audio_output);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: play tone");
}

// Wraps the real LvglUserActivitySource with a settable override - it
// has no separate fake backend to begin with (unlike HostBatteryReader/
// HostNetworkStatus, real UserActivitySource is identical on both
// targets, see ui/lvgl_user_activity_source.h), and LVGL has no "pretend
// N minutes passed" API, only lv_display_trigger_activity() (resets to
// just-active). Stays local debug scaffolding here rather than a second
// real platform/host/ backend, same reasoning force_ota_failure below
// already follows.
class DebugOverridableUserActivitySource : public homedeck::UserActivitySource {
public:
    enum class ForcedLevel { kNone, kIdle, kSleeping };

    explicit DebugOverridableUserActivitySource(homedeck::UserActivitySource& real) : real_(real) {}

    uint32_t MillisecondsSinceLastActivity() const override {
        switch (forced_) {
            case ForcedLevel::kIdle:
                return kForcedIdleOnlyMs;
            case ForcedLevel::kSleeping:
                return kForcedSleepingMs;
            case ForcedLevel::kNone:
            default:
                return real_.MillisecondsSinceLastActivity();
        }
    }

    void SetForced(ForcedLevel level) { forced_ = level; }

private:
    // Past kIdleTimeoutMs but short of kSleepTimeoutMs (see
    // core/power_manager.cpp) so this level parks at Idle rather than
    // cascading into Sleeping on a later tick - a known, accepted
    // coupling to those placeholder values, same convention
    // kForcedSleepingMs below already follows.
    static constexpr uint32_t kForcedIdleOnlyMs = 60000;
    // Past any real placeholder timeout, including Sleep's - see
    // core/power_manager.cpp.
    static constexpr uint32_t kForcedSleepingMs = 24u * 60 * 60 * 1000;

    homedeck::UserActivitySource& real_;
    ForcedLevel forced_ = ForcedLevel::kNone;
};

void OnTestTriggerIdleClicked(lv_event_t* e) {
    auto* source = static_cast<DebugOverridableUserActivitySource*>(lv_event_get_user_data(e));
    source->SetForced(DebugOverridableUserActivitySource::ForcedLevel::kIdle);
}

void CreateTestTriggerIdleButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestTriggerIdleClicked, LV_EVENT_CLICKED, &source);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger idle");
}

void OnTestTriggerSleepingClicked(lv_event_t* e) {
    auto* source = static_cast<DebugOverridableUserActivitySource*>(lv_event_get_user_data(e));
    source->SetForced(DebugOverridableUserActivitySource::ForcedLevel::kSleeping);
}

void CreateTestTriggerSleepingButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestTriggerSleepingClicked, LV_EVENT_CLICKED, &source);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger sleeping");
}

void OnTestTriggerActiveClicked(lv_event_t* e) {
    auto* source = static_cast<DebugOverridableUserActivitySource*>(lv_event_get_user_data(e));
    source->SetForced(DebugOverridableUserActivitySource::ForcedLevel::kNone);
    // Clears the override, but the underlying real clock needs to be
    // fresh too - otherwise it could still read as idle on the very
    // next tick if the real SDL window hasn't actually been touched
    // recently.
    lv_display_trigger_activity(nullptr);
}

void CreateTestTriggerActiveButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_add_event_cb(button, OnTestTriggerActiveClicked, LV_EVENT_CLICKED, &source);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: trigger active");
}

}  // namespace

// The dashboard shell, StatusBar, and DashboardGrid widget framework -
// see dashboard.md's Status for what's real. Plus a minimal Navigation
// manager and persistent home affordance, proven via a deliberately
// throwaway second screen (see screens/placeholder_screen.h).
int main() {
    homedeck::EventBus event_bus;
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus, ResolveWindowZoom());

    // DashboardScreen (whose StatusBar and ClockWidget members are both
    // ClockTickEvent subscribers) must exist before Clock (the
    // publisher) - Clock publishes one tick immediately at construction
    // (see clock.cpp) so subscribers get a real value right away rather
    // than sitting blank for up to a full tick period; that only
    // reaches anyone if the subscription already exists when it
    // happens.
    homedeck::HostBatteryReader battery_reader;
    homedeck::HostNetworkStatus network_status;
    homedeck::HostAudioOutput audio_output;
    // Safe to construct here - UiTask's constructor above already ran
    // lv_init(), and neither constructor otherwise touches LVGL/BSP
    // state beyond that (see ui/lvgl_user_activity_source.h).
    homedeck::HostDisplayBrightness display_brightness;
    homedeck::LvglUserActivitySource real_user_activity_source;
    DebugOverridableUserActivitySource user_activity_source(real_user_activity_source);

    // Moved ahead of the dashboard's construction below (unlike Logger
    // further down, which doesn't need to be) so WeatherWidget can be
    // constructed alongside ClockWidget/NetworkStatusWidget - see
    // homedeck.cpp's identical reordering note (no equivalent OS-init
    // constraint here, HostSettingsStore/HostSecretStore are plain
    // filesystem paths).
    std::filesystem::path storage_root = std::filesystem::temp_directory_path() / "homedeck_simulator";
    homedeck::HostSettingsStore settings_store(storage_root);
    homedeck::HostCacheStore cache_store(storage_root);
    homedeck::HostSecretStore secret_store(storage_root);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::HostHttpClient http_client;
    homedeck::OpenMeteoWeatherProvider weather_provider(http_client, storage, event_bus);

    homedeck::DashboardScreen dashboard(event_bus, battery_reader, network_status);
    homedeck::ClockWidget clock_widget(dashboard.Grid().Container(), event_bus);
    dashboard.Grid().AddWidget(clock_widget);
    homedeck::NetworkStatusWidget network_status_widget(dashboard.Grid().Container(), event_bus, network_status);
    dashboard.Grid().AddWidget(network_status_widget);
    homedeck::WeatherWidget weather_widget(dashboard.Grid().Container(), event_bus, weather_provider);
    dashboard.Grid().AddWidget(weather_widget);
    homedeck::NotificationWidget notification_widget(dashboard.Grid().Container(), event_bus);
    dashboard.Grid().AddWidget(notification_widget);

    homedeck::Navigation navigation("dashboard", dashboard.Root());

    // Touch UI fallback for initial Wi-Fi setup (see
    // docs/architecture/networking.md#initial-wi-fi-provisioning) - built
    // and exercisable here like any other screen, per this project's
    // "simulator as primary UI dev environment" philosophy, even though
    // there's no real esp_wifi call to make on this target.
    homedeck::WifiSetupScreen wifi_setup_screen(
        event_bus, battery_reader, network_status, [](const std::string& ssid, const std::string& password) {
            std::printf("Wi-Fi setup submitted (no-op in simulator): SSID=%s\n", ssid.c_str());
        });
    // No real SoftAP here to derive these from - a fixed placeholder is
    // enough to exercise the layout (see wifi_setup_screen.h's SetApInfo).
    wifi_setup_screen.SetApInfo("HomeDeck-SIM01", "192.168.4.1");
    navigation.Register("wifi-setup", wifi_setup_screen.Root());
    CreateTestBackToDashboardButton(wifi_setup_screen.Root(), navigation);

    lv_obj_t* test_button_panel = CreateTestButtonPanel(dashboard.Root());
    CreateTestWifiSetupNavButton(test_button_panel, navigation);
    CreateTestWifiDisconnectButton(test_button_panel, network_status);
    CreateTestPlayToneButton(test_button_panel, audio_output);
    CreateTestLowBatteryButton(test_button_panel, battery_reader);
    CreateTestCriticalBatteryButton(test_button_panel, battery_reader);
    CreateTestExternalPowerButton(test_button_panel, battery_reader);
    CreateTestBatteryPresentButton(test_button_panel, battery_reader);
    // Declared here, not narrower - captured by reference into
    // ota_writer.write_image below, which must stay valid for the
    // server's lifetime.
    bool force_ota_failure = false;
    CreateTestForceOtaFailureButton(test_button_panel, force_ota_failure);
    CreateTestTriggerIdleButton(test_button_panel, user_activity_source);
    CreateTestTriggerSleepingButton(test_button_panel, user_activity_source);
    CreateTestTriggerActiveButton(test_button_panel, user_activity_source);

    // Read once at startup, before constructing the objects these seed -
    // see quick_settings_panel.h for where they're subsequently
    // controlled. Falls back to the pre-existing hardcoded defaults
    // (100/70) if unset (first boot) or unparsable.
    int initial_active_brightness_percent = ReadIntSetting(storage, "power", "brightness", 100);
    int initial_volume_percent = ReadIntSetting(storage, "audio", "volume", 70);

    // NotificationBanner and NotificationSound must exist before
    // LowBatteryMonitor, which must exist before Clock (the
    // ClockTickEvent publisher) - the same "subscriber before publisher"
    // ordering StatusBar's own comment above already explains, extended
    // one step further: LowBatteryMonitor itself publishes
    // NotificationEvent, so both subscribers need to already be
    // registered before that first tick could fire one.
    homedeck::NotificationBanner notification_banner(event_bus);
    homedeck::NotificationSound notification_sound(event_bus, audio_output, initial_volume_percent);
    homedeck::LowBatteryMonitor low_battery_monitor(event_bus, battery_reader);
    // Same ordering rule as LowBatteryMonitor above - also a
    // ClockTickEvent subscriber, and also publishes NotificationEvent,
    // so it needs NotificationBanner/NotificationSound already
    // registered too.
    homedeck::CriticalBatteryMonitor critical_battery_monitor(event_bus, battery_reader);
    // Same "subscriber before publisher" ordering as LowBatteryMonitor -
    // NetworkStatusMonitor is also a ClockTickEvent subscriber.
    homedeck::NetworkStatusMonitor network_status_monitor(event_bus, network_status);
    // Grouped here for readability, not a real ordering dependency on
    // NotificationBanner/NotificationSound - see homedeck.cpp's identical
    // note. A second SteadyTimeSource instance (stateless, see
    // platform/steady_time_source.h) rather than sharing auth_time_source
    // below, which is constructed later and named for AdminAuthService
    // specifically.
    homedeck::SteadyTimeSource power_time_source;
    homedeck::PowerManager power_manager(event_bus, user_activity_source, display_brightness,
                                          power_time_source, initial_active_brightness_percent);
    // Needs PowerManager/NotificationSound/Storage all already
    // constructed above - see quick_settings_panel.h.
    homedeck::QuickSettingsPanel quick_settings_panel(event_bus, power_manager, notification_sound, storage);

    homedeck::HostTimeSource time_source;
    homedeck::Clock clock(time_source, event_bus);

    // Storage (constructed earlier above, see its own comment there)
    // backs both AdminAuthService's password hash - a fixed location
    // under the OS temp directory rather than a fresh one per run, so
    // the simulator's admin password persists across restarts the same
    // way NVS would on real hardware, instead of demanding first-login
    // setup again every time the simulator relaunches - and Logger here.
    homedeck::Logger logger(storage, time_source);
    CreateTestLogEntryButton(test_button_panel, logger);
    // A monotonic clock, not the shared wall-clock time_source above -
    // matches firmware's identical choice (see
    // platform/steady_time_source.h) so AdminAuthService behaves the
    // same on both targets, rather than the simulator's forgiving host
    // wall clock accidentally masking a real firmware-only issue.
    homedeck::SteadyTimeSource auth_time_source;
    homedeck::AdminAuthService admin_auth(storage, auth_time_source);

    // The Web Management UI's server primitive (see
    // docs/architecture/web-ui.md#status) - the built Svelte/Vite
    // scaffold plus admin auth, settings, and diagnostics.
    homedeck::HostHttpServer web_server;
    // Read once at startup, not per-request - matches firmware's
    // EMBED_FILES approach (data available for the process's lifetime),
    // see homedeck.cpp's identical wiring and
    // docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage.
    // HOMEDECK_WEBUI_DIR is a source-tree-relative path (webui/dist)
    // baked in by CMakeLists.txt, so this works regardless of the
    // simulator's current working directory when launched.
    std::vector<homedeck::StaticAsset> webui_assets;
    for (const auto& [path, filename, content_type] :
         {std::tuple{"/", "index.html", "text/html"}, std::tuple{"/app.js", "app.js", "text/javascript"},
          std::tuple{"/app.css", "app.css", "text/css"}}) {
        auto content = homedeck::ReadFile(std::filesystem::path(HOMEDECK_WEBUI_DIR) / filename);
        if (content.has_value()) {
            webui_assets.push_back({path, content_type, *content});
        } else {
            std::printf("Warning: could not read %s/%s\n", HOMEDECK_WEBUI_DIR, filename);
        }
    }
    homedeck::ServeStaticFiles(web_server, std::move(webui_assets));
    homedeck::RegisterAdminAuthRoutes(web_server, admin_auth);
    // Diagnostics needs real Storage-resident data to read (see
    // firmware/main/crash_diagnostics.cpp for the real, firmware-only
    // equivalent) - mock values here so the Web UI's diagnostics page is
    // exercisable in dev without real hardware, per
    // docs/architecture/diagnostics.md's "Firmware-only mechanism" note.
    storage.SetSetting("core", "reset_reason", 1, "power-on");
    storage.SetSetting("core", "has_core_dump", 1, "true");
    homedeck::RegisterDiagnosticsRoutes(web_server, storage, admin_auth, battery_reader, logger,
                                         []() -> std::optional<std::string> {
        return std::string(
            "This is a simulator-only stub core dump for Web UI development - "
            "see docs/architecture/diagnostics.md.");
    });
    // No real OTA partition here - matches firmware's 4MB ota_0/ota_1
    // sizing (see docs/decisions/ADR-0017-partition-table.md) so an
    // oversized-upload rejection is exercisable identically on both
    // targets. write_image reads the body (exercising the same read
    // path as firmware) and discards it - no real partition writes, per
    // docs/architecture/simulator.md's OTA mock description.
    homedeck::OtaWriter ota_writer{
        .max_image_size = []() -> size_t { return 4 * 1024 * 1024; },
        .write_image =
            [&force_ota_failure](const std::string& /*image*/) -> bool { return !force_ota_failure; },
        .running_version = []() -> std::string { return "simulator-dev"; },
    };
    homedeck::RegisterOtaRoutes(web_server, event_bus, admin_auth, battery_reader, ota_writer,
                                 []() { std::printf("OTA reboot requested (no-op in simulator)\n"); });
    // No device-name-changed callback - there's no mDNS to re-announce
    // on the simulator, so a device name change just persists to
    // storage like any other setting (see settings_routes.h's own
    // comment on why the callback is optional).
    homedeck::RegisterSettingsRoutes(web_server, storage, admin_auth);
    homedeck::RegisterWeatherRoutes(web_server, http_client, weather_provider, admin_auth);
    uint16_t web_port = ResolveWebPort();
    if (web_server.Start(web_port)) {
        std::printf("Web UI listening on http://localhost:%u/\n", web_port);
        logger.Log(homedeck::LogLevel::kInfo, "web_server", "Listening on port " + std::to_string(web_port));
    } else {
        std::printf("Web UI failed to start on port %u\n", web_port);
        logger.Log(homedeck::LogLevel::kError, "web_server", "Failed to start on port " + std::to_string(web_port));
    }

    ui_task.Run();
}
