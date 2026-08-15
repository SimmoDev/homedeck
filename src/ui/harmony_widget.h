#pragma once

#include "core/event_bus.h"
#include "core/harmony_connection.h"
#include "lvgl.h"
#include "ui/navigation.h"
#include "ui/widget.h"

namespace homedeck {

// Current Harmony activity (Harmony module) - the dashboard grid widget
// docs/architecture/dashboard.md's Widget system section already names
// as an example. Tapping it opens ActivitiesScreen
// (src/ui/screens/activities_screen.h) - the first use of
// Widget::OnTap(), per that same doc's Status section and
// roadmap.md's M2 Widget framework item.
class HarmonyWidget : public Widget {
public:
    HarmonyWidget(lv_obj_t* parent, EventBus& event_bus, HarmonyConnection& harmony_connection,
                  Navigation& navigation);

    lv_obj_t* Root() const override { return root_; }
    // Same reasoning as ClockWidget/NetworkStatusWidget - a 1x1 (170px)
    // cell truncates an activity name at Montserrat 24 too easily.
    int ColumnSpan() const override { return 2; }

    void OnTap() override;

private:
    void Refresh();

    HarmonyConnection& harmony_connection_;
    Navigation& navigation_;

    lv_obj_t* root_;
    lv_obj_t* label_;

    EventBus::ScopedSubscription state_sub_;
    EventBus::ScopedSubscription config_sub_;
    EventBus::ScopedSubscription activity_sub_;
};

}  // namespace homedeck
