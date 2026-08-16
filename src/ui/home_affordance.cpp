#include "ui/home_affordance.h"

namespace homedeck {

namespace {

void OnHomeClicked(lv_event_t* e) {
    auto* navigation = static_cast<Navigation*>(lv_event_get_user_data(e));
    navigation->GoHome();
}

}  // namespace

lv_obj_t* CreateHomeAffordance(lv_obj_t* parent, Navigation& navigation) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_align(button, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    // See StatusBar's own FLOATING comment - same fix, same reason: this
    // button is a child of each screen's scrolling root_, and without
    // this it scrolls out of view/drags with overscroll instead of
    // staying pinned as a fixed affordance.
    lv_obj_add_flag(button, LV_OBJ_FLAG_FLOATING);
    // Grey, not the default theme blue every action/remote button already
    // uses (CreateRemoteButton, ActivitiesScreen's activity buttons) -
    // this is navigation chrome, not an action, and blended in against
    // them otherwise. Plain placeholder differentiation, not considered
    // theme styling - see ActivitiesScreen's own kCurrentActivityPalette
    // comment for that same "real theme work is M7 scope" precedent.
    lv_obj_set_style_bg_color(button, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_add_event_cb(button, OnHomeClicked, LV_EVENT_CLICKED, &navigation);

    lv_obj_t* icon = lv_label_create(button);
    lv_label_set_text(icon, LV_SYMBOL_HOME);

    return button;
}

}  // namespace homedeck
