#include "ui/screens/dashboard_screen.h"

#include <cstdio>
#include <ctime>

namespace homedeck {

namespace {

// localtime_r is POSIX; matches Linux being the only currently-verified
// simulator platform (see DEVELOPMENT.md) - revisit if/when Windows
// support is actually pursued.
void FormatTime(std::chrono::system_clock::time_point time, char* buffer, size_t size) {
    std::time_t t = std::chrono::system_clock::to_time_t(time);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::strftime(buffer, size, "%H:%M:%S\n%a %d %b %Y", &tm);
}

}  // namespace

DashboardScreen::DashboardScreen(lv_obj_t* parent, EventBus& event_bus,
                                  BatteryReader& battery_reader) {
    clock_label_ = lv_label_create(parent);
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, 0);

    battery_label_ = lv_label_create(parent);
    char battery_text[16];
    std::snprintf(battery_text, sizeof(battery_text), "%d%%", battery_reader.ReadPercent());
    lv_label_set_text(battery_label_, battery_text);
    lv_obj_align(battery_label_, LV_ALIGN_TOP_RIGHT, -16, 16);

    clock_subscription_ =
        event_bus.SubscribeUi<ClockTickEvent>([this](const ClockTickEvent& event) {
            char text[32];
            FormatTime(event.time, text, sizeof(text));
            lv_label_set_text(clock_label_, text);
        });
}

}  // namespace homedeck
