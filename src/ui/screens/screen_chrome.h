#pragma once

#include "lvgl.h"
#include "ui/navigation.h"

namespace homedeck {

// The screen-level scaffolding a module's Touch UI screens share
// (CLAUDE.md's "avoid duplicate code"): the body-font setting on root, a
// padded flex-column container below StatusBar, a title label, a
// "not connected yet" hint label whose exact wording the caller passes
// (each screen owns toggling hint_label/content_container visibility
// itself, based on its own module's connection snapshot), a scrollable
// content_container, and the persistent home affordance.
//
// Factored out of Harmony's ActivitiesScreen/DevicesScreen (M3) and
// reused by Kodi's screens (M4) - nothing here is module-specific.
struct ScreenChrome {
    lv_obj_t* container;
    lv_obj_t* hint_label;
    lv_obj_t* content_container;
    lv_obj_t* home_button;
};

// title's children are created in this order: title label, hint_label,
// content_container - a caller needing to insert its own content between
// the title and hint_label (e.g. ActivitiesScreen's status_label_)
// creates it after this call and repositions it with
// lv_obj_move_to_index(label, 1), rather than this function taking on a
// caller-specific insertion hook.
ScreenChrome CreateScreenChrome(lv_obj_t* root, const char* title, const char* hint_text, Navigation& navigation);

// A standing status label positioned between the title and hint_label
// (index 1) - see CreateScreenChrome()'s own insertion-order comment.
// Starts empty (LVGL defaults a new label's text to "Text" otherwise);
// each screen owns its own event-subscription wiring and status text.
lv_obj_t* CreateChromeStatusLabel(lv_obj_t* container);

}  // namespace homedeck
