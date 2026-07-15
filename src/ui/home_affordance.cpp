#include "ui/home_affordance.h"

namespace homedeck {

namespace {

void OnHomeClicked(lv_event_t* e) {
    auto* navigation = static_cast<Navigation*>(lv_event_get_user_data(e));
    navigation->GoHome();
}

}  // namespace

void CreateHomeAffordance(lv_obj_t* parent, Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_add_event_cb(button, OnHomeClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* icon = lv_label_create(button);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
}

}  // namespace homedeck
