#pragma once

#include "lvgl.h"
#include "ui/navigation.h"

namespace homedeck {

// The screen-level scaffolding ActivitiesScreen/DevicesScreen both built
// identically before this was factored out (CLAUDE.md's "avoid duplicate
// code"): the body-font setting on root, a padded flex-column container
// below StatusBar, a title label, a "not connected yet" hint label (each
// screen owns toggling hint_label/list_container's visibility itself,
// based on its own HarmonyConnectionSnapshot::has_config check), a
// scrollable list_container, and the persistent home affordance.
struct HarmonyScreenChrome {
    lv_obj_t* container;
    lv_obj_t* hint_label;
    lv_obj_t* list_container;
    lv_obj_t* home_button;
};

// title's children are created in this order: title label, hint_label,
// list_container - a caller needing to insert its own content between the
// title and hint_label (ActivitiesScreen's status_label_) creates it
// after this call and repositions it with lv_obj_move_to_index(label, 1),
// rather than this function taking on a caller-specific insertion hook.
HarmonyScreenChrome CreateHarmonyScreenChrome(lv_obj_t* root, const char* title, Navigation& navigation);

}  // namespace homedeck
