#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/host/battery_reader.h"
#include "platform/host/time_source.h"
#include "screens/placeholder_screen.h"
#include "ui/navigation.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/ui_task.h"

// Matches the Tab5's confirmed 1280x720 display (see docs/architecture/hardware.md).
static constexpr int32_t kWindowWidth = 1280;
static constexpr int32_t kWindowHeight = 720;

namespace {

// Temporary test-only wiring proving Navigation/the persistent home
// affordance (see docs/roadmap.md's M1 item) - there's no real
// "launch an app" affordance on the dashboard yet, so this stands in for
// one. Removed once a real navigation trigger (an app icon, a widget
// tap) exists; kept out of DashboardScreen itself so real product code
// stays free of throwaway test scaffolding.
void OnTestNavClicked(lv_event_t* e) {
    auto* navigation = static_cast<homedeck::Navigation*>(lv_event_get_user_data(e));
    navigation->GoTo("placeholder");
}

void CreateTestNavButton(lv_obj_t* parent, homedeck::Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(button, OnTestNavClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Test: go to placeholder screen");
}

}  // namespace

// The initial dashboard shell per docs/roadmap.md's M1 items - Core-only
// widgets (clock/date, battery), no pluggable widget framework or grid
// layout yet (both M2 scope, per dashboard.md's Status). Plus a minimal
// Navigation manager and persistent home affordance, proven via a
// deliberately throwaway second screen (see screens/placeholder_screen.h).
int main() {
    homedeck::EventBus event_bus;
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus);

    // DashboardScreen (the ClockTickEvent subscriber) must exist before
    // Clock (the publisher) - Clock publishes one tick immediately at
    // construction (see clock.cpp) so the display doesn't show LVGL's
    // placeholder text until the first periodic tick; that only reaches
    // anyone if the subscription already exists when it happens.
    homedeck::HostBatteryReader battery_reader;
    homedeck::DashboardScreen dashboard(event_bus, battery_reader);

    homedeck::Navigation navigation("dashboard", dashboard.Root());

    PlaceholderScreen placeholder(navigation);
    navigation.Register("placeholder", placeholder.Root());

    CreateTestNavButton(dashboard.Root(), navigation);

    homedeck::HostTimeSource time_source;
    homedeck::Clock clock(time_source, event_bus);

    ui_task.Run();
}
