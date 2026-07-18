#pragma once

#include "core/event_bus.h"
#include "lvgl.h"
#include "platform/battery_reader.h"
#include "ui/navigation.h"
#include "ui/status_bar.h"

// A deliberately minimal, throwaway second screen - exists only to prove
// the Navigation manager + persistent home affordance mechanism actually
// works (see docs/roadmap.md's M1 "Persistent home affordance" item).
// Not real product UI - simulator-only, following this project's
// throwaway-scaffold pattern for proving out mechanisms before real UI
// exists. Replaced once a genuine second screen exists (an M2 settings
// screen, or the first M3 module screen). Also includes StatusBar, since
// that mechanism (present on *every* screen, per ADR-0008) needs proving
// out across more than one screen too, the same reasoning that justified
// proving out Navigation/the home affordance here.
class PlaceholderScreen {
public:
    PlaceholderScreen(homedeck::Navigation& navigation, homedeck::EventBus& event_bus,
                       homedeck::BatteryReader& battery_reader);

    lv_obj_t* Root() const { return root_; }

private:
    lv_obj_t* root_;
    homedeck::StatusBar status_bar_;
};
