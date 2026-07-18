#include "placeholder_screen.h"

#include "ui/home_affordance.h"

PlaceholderScreen::PlaceholderScreen(homedeck::Navigation& navigation, homedeck::EventBus& event_bus,
                                      homedeck::BatteryReader& battery_reader)
    : root_(lv_obj_create(nullptr)), status_bar_(root_, event_bus, battery_reader) {
    lv_obj_t* label = lv_label_create(root_);
    lv_label_set_text(label, "Second screen (placeholder)");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    homedeck::CreateHomeAffordance(root_, navigation);
}
