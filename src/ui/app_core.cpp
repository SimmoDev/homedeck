#include "ui/app_core.h"

#include <charconv>

namespace homedeck {

namespace {

// Shared by NotificationSound/PowerManager's initial volume/brightness
// below - std::from_chars, not std::stoi, matches Storage's own internal
// parsing (see storage.cpp's Decode()) for the same firmware-builds-
// without-exceptions reason.
int ReadIntSetting(Storage& storage, const char* module_id, const char* key, int fallback) {
    std::optional<VersionedValue> setting = storage.GetSetting(module_id, key);
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

}  // namespace

AppCore::AppCore(EventBus& event_bus, Dependencies deps)
    : storage_(deps.settings_store, deps.cache_store, deps.secret_store),
      http_client_(deps.http_client),
      weather_provider_(deps.http_client, storage_, event_bus),
      dashboard_(event_bus, deps.battery_reader, deps.network_status),
      clock_widget_(dashboard_.Grid().Container(), event_bus),
      network_status_widget_(dashboard_.Grid().Container(), event_bus, deps.network_status),
      weather_widget_(dashboard_.Grid().Container(), event_bus, weather_provider_),
      notification_widget_(dashboard_.Grid().Container(), event_bus),
      notification_banner_(event_bus),
      notification_sound_(event_bus, deps.audio_output, ReadIntSetting(storage_, "audio", "volume", 70)),
      low_battery_monitor_(event_bus, deps.battery_reader),
      critical_battery_monitor_(event_bus, deps.battery_reader),
      network_status_monitor_(event_bus, deps.network_status),
      power_manager_(event_bus, deps.user_activity_source, deps.display_brightness, power_time_source_,
                     ReadIntSetting(storage_, "power", "brightness", 100)),
      quick_settings_panel_(event_bus, power_manager_, notification_sound_, storage_),
      navigation_("dashboard", dashboard_.Root()),
      wifi_setup_screen_(event_bus, deps.battery_reader, deps.network_status, deps.wifi_submit),
      clock_(deps.time_source, event_bus),
      logger_(storage_, deps.time_source),
      admin_auth_(storage_, auth_time_source_) {
    // AddWidget order matches each widget's declaration order above -
    // no ordering requirement of its own, just kept consistent for
    // readability.
    dashboard_.Grid().AddWidget(clock_widget_);
    dashboard_.Grid().AddWidget(network_status_widget_);
    dashboard_.Grid().AddWidget(weather_widget_);
    dashboard_.Grid().AddWidget(notification_widget_);

    navigation_.Register("wifi-setup", wifi_setup_screen_.Root());

    RegisterAdminAuthRoutes(deps.http_server, admin_auth_);
    RegisterDiagnosticsRoutes(deps.http_server, storage_, admin_auth_, deps.battery_reader, logger_,
                               deps.read_core_dump);
    RegisterOtaRoutes(deps.http_server, event_bus, admin_auth_, deps.battery_reader, deps.ota_writer,
                       deps.ota_reboot);
    // Forwards to on_device_name_changed_ so SetOnDeviceNameChanged() can
    // be called after this constructor returns (see its own comment for
    // why) - RegisterSettingsRoutes bakes whatever callback it's given
    // into the handler it registers right now, so a direct
    // deps.on_device_name_changed wouldn't be able to reference anything
    // constructed later in this same constructor (namely logger_).
    RegisterSettingsRoutes(deps.http_server, storage_, admin_auth_, [this](const std::string& value) {
        return on_device_name_changed_ ? on_device_name_changed_(value) : true;
    });
    RegisterWeatherRoutes(deps.http_server, http_client_, weather_provider_, admin_auth_);
    RegisterWifiRoutes(deps.http_server, admin_auth_, deps.wifi_reset);
}

void AppCore::Start() { clock_.Start(); }

}  // namespace homedeck
