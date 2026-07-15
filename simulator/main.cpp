#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/host/battery_reader.h"
#include "platform/host/time_source.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/ui_task.h"

// Matches the Tab5's confirmed 1280x720 display (see docs/architecture/hardware.md).
static constexpr int32_t kWindowWidth = 1280;
static constexpr int32_t kWindowHeight = 720;

// The initial dashboard shell per docs/roadmap.md's M1 items - Core-only
// widgets (clock/date, battery), no pluggable widget framework or grid
// layout yet (both M2 scope, per dashboard.md's Status).
int main() {
    homedeck::EventBus event_bus;
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus);

    // DashboardScreen (the ClockTickEvent subscriber) must exist before
    // Clock (the publisher) - Clock publishes one tick immediately at
    // construction (see clock.cpp) so the display doesn't show LVGL's
    // placeholder text until the first periodic tick; that only reaches
    // anyone if the subscription already exists when it happens.
    homedeck::HostBatteryReader battery_reader;
    homedeck::DashboardScreen dashboard(ui_task.ActiveScreen(), event_bus, battery_reader);

    homedeck::HostTimeSource time_source;
    homedeck::Clock clock(time_source, event_bus);

    ui_task.Run();
}
