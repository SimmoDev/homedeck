#include "ui/weather_widget.h"

#include "ui/theme.h"
#include "ui/widget_tile.h"

#include <cstdio>

namespace homedeck {

namespace {

// WMO weather interpretation codes, per Open-Meteo's own documentation
// (https://open-meteo.com/en/docs - "WMO Weather interpretation codes").
// Grouped to the coarser categories that matter for a compact widget,
// not every individual code.
const char* WeatherCodeText(int code) {
    switch (code) {
        case 0:
            return "Clear sky";
        case 1:
        case 2:
        case 3:
            return "Partly cloudy";
        case 45:
        case 48:
            return "Fog";
        case 51:
        case 53:
        case 55:
        case 56:
        case 57:
            return "Drizzle";
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
            return "Rain";
        case 71:
        case 73:
        case 75:
        case 77:
            return "Snow";
        case 80:
        case 81:
        case 82:
            return "Rain showers";
        case 85:
        case 86:
            return "Snow showers";
        case 95:
        case 96:
        case 99:
            return "Thunderstorm";
        default:
            return "Unknown";
    }
}

// Confirmed present in Montserrat 24's glyph set (the sparse extended
// range starting at U+00B0 - same font StatusBar/ClockWidget already
// use), not assumed - the built-in LVGL font doesn't cover Latin-1
// Supplement in general, but this one specific codepoint is included
// alongside the LV_SYMBOL_* private-use-area icons.
constexpr const char kDegree[] = "\xC2\xB0";

void RefreshWeatherLabels(lv_obj_t* condition_label, lv_obj_t* location_label, const WeatherState& state) {
    if (!state.configured) {
        lv_label_set_text(condition_label, "Weather");
        lv_label_set_text(location_label, "Set a location in Settings");
        return;
    }
    if (!state.has_reading) {
        lv_label_set_text(condition_label, "Fetching...");
        lv_label_set_text(location_label, state.display_name.c_str());
        return;
    }

    char text[48];
    std::snprintf(text, sizeof(text), "%.0f%sC  %s%s", state.temperature_c, kDegree,
                  WeatherCodeText(state.weather_code), state.live ? "" : " (cached)");
    lv_label_set_text(condition_label, text);
    lv_label_set_text(location_label, state.display_name.c_str());
}

}  // namespace

WeatherWidget::WeatherWidget(lv_obj_t* parent, EventBus& event_bus, WeatherProvider& weather_provider) {
    root_ = CreateWidgetTileRoot(parent);

    condition_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(condition_label_, kBodyFont, 0);

    location_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(location_label_, kBodyFont, 0);
    // display_name is an unbounded name/admin1/country concatenation
    // (see Settings.svelte's selectWeatherLocation), easily wide enough
    // to overflow this tile - same truncate-with-ellipsis treatment as
    // NetworkStatusWidget's SSID label, for the same reason (wrapping
    // would push content out of this tile's fixed height).
    lv_label_set_long_mode(location_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(location_label_, LV_PCT(100));
    lv_obj_set_style_text_align(location_label_, LV_TEXT_ALIGN_CENTER, 0);

    RefreshWeatherLabels(condition_label_, location_label_, weather_provider.Snapshot());

    weather_subscription_ = event_bus.SubscribeUi<WeatherUpdatedEvent>(
        [this, &weather_provider](const WeatherUpdatedEvent&) {
            RefreshWeatherLabels(condition_label_, location_label_, weather_provider.Snapshot());
        });
}

}  // namespace homedeck
