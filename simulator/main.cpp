#include "core/clock.h"
#include "core/event_bus.h"
#include "platform/host/battery_reader.h"
#include "platform/host/http_server.h"
#include "platform/host/time_source.h"
#include "screens/placeholder_screen.h"
#include "ui/navigation.h"
#include "ui/screens/dashboard_screen.h"
#include "ui/ui_task.h"

#include <cstdio>
#include <cstdlib>

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
    homedeck::UiTask ui_task(kWindowWidth, kWindowHeight, event_bus, ResolveWindowZoom());

    // DashboardScreen (whose StatusBar member is the ClockTickEvent
    // subscriber) must exist before Clock (the publisher) - Clock
    // publishes one tick immediately at construction (see clock.cpp) so
    // the display doesn't show LVGL's placeholder text until the first
    // periodic tick; that only reaches anyone if the subscription
    // already exists when it happens.
    homedeck::HostBatteryReader battery_reader;
    homedeck::DashboardScreen dashboard(event_bus, battery_reader);

    homedeck::Navigation navigation("dashboard", dashboard.Root());

    PlaceholderScreen placeholder(navigation, event_bus, battery_reader);
    navigation.Register("placeholder", placeholder.Root());

    CreateTestNavButton(dashboard.Root(), navigation);

    homedeck::HostTimeSource time_source;
    homedeck::Clock clock(time_source, event_bus);

    // The Web Management UI's server primitive (see
    // docs/architecture/web-ui.md#status) - just a placeholder route for
    // now. Auth, real settings/diagnostics pages, and the Svelte/Vite
    // frontend are all future passes.
    homedeck::HostHttpServer web_server;
    web_server.RegisterHandler(homedeck::HttpMethod::kGet, "/", [](const homedeck::HttpRequest&) {
        return homedeck::HttpResponse{200, "text/plain", "HomeDeck Web UI - coming soon"};
    });
    uint16_t web_port = ResolveWebPort();
    if (web_server.Start(web_port)) {
        std::printf("Web UI listening on http://localhost:%u/\n", web_port);
    } else {
        std::printf("Web UI failed to start on port %u\n", web_port);
    }

    ui_task.Run();
}
