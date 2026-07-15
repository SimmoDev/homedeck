#include "core/event_bus.h"
#include "platform/timer.h"
#include "screens/heartbeat_screen.h"
#include "ui/ui_task.h"

#include <chrono>

// Matches the Tab5's confirmed 1280x720 display (see docs/architecture/hardware.md).
static constexpr int32_t kWindowWidth = 1280;
static constexpr int32_t kWindowHeight = 720;

// The Core Concurrency Abstraction + EventBus + dedicated-UI-task app
// per docs/roadmap.md's M1 items. HeartbeatEvent/HeartbeatScreenController
// are a deliberately trivial proof of the mechanism, not real product
// UI - the dashboard shell replaces this next.
int main() {
    homedeck::EventBus event_bus;
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus);

    HeartbeatScreenController heartbeat_screen(ui_task.ActiveScreen(), event_bus);

    int count = 0;
    homedeck::Timer heartbeat_timer(
        "heartbeat", std::chrono::seconds(1),
        [&count, &event_bus]() { event_bus.Publish(HeartbeatEvent{++count}); });

    ui_task.Run();
}
