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
// Shared by Harmony's and Kodi's screens - nothing here is
// module-specific.
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

// A hidden-until-shown detail/list-level view container, as used by every
// Kodi browse screen's non-top-level view (KodiTvShowsScreen's
// seasons_container_, KodiMusicScreen's albums_container_, ...): 90% width,
// flex column, horizontally centered/vertically top-aligned content,
// hidden until the owning screen shows it. pad_row is
// passed explicitly because call sites use different spacing (8 for
// DevicesScreen and KodiLiveTvScreen, 12 for the others).
lv_obj_t* CreateChromeDetailContainer(lv_obj_t* parent, int32_t pad_row);

// A hidden, centered, word-wrapping note a Kodi control screen shows while
// KodiSnapshot::library_busy is set (Kodi answers nothing else while it
// finishes a slow listing). Created as `parent`'s first child, so it sits
// above the controls it explains.
lv_obj_t* CreateKodiBusyHint(lv_obj_t* parent);

// A left-aligned, word-wrapping heading label (e.g. a selected show's
// title atop its season list) - full width, with a top margin separating
// it from the "back" button a detail container's own first child usually
// is.
lv_obj_t* CreateChromeHeadingLabel(lv_obj_t* parent);

}  // namespace homedeck
