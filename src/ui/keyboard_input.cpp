#include "ui/keyboard_input.h"

#include "ui/theme.h"

namespace homedeck {

namespace {

void OnTextAreaFocused(lv_event_t* e) {
    auto* keyboard = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    auto* textarea = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_keyboard_set_textarea(keyboard, textarea);
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

// The keyboard widget sends READY/CANCEL to both itself and its attached
// textarea - listening on the keyboard itself means one registration
// here covers every textarea AttachTo() is ever called with.
void OnKeyboardReadyOrCancel(lv_event_t* e) {
    auto* keyboard = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_keyboard_set_textarea(keyboard, nullptr);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

OnScreenKeyboard::OnScreenKeyboard(lv_obj_t* parent) {
    keyboard_ = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard_, LV_PCT(100), LV_PCT(40));
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    // lv_buttonmatrix (which the keyboard is built on) draws button/key
    // labels via LV_PART_ITEMS, a separate style target from LV_PART_MAIN
    // - a parent's inherited text_font never reaches them, so this has to
    // be set here directly rather than left to whatever screen owns this
    // instance.
    lv_obj_set_style_text_font(keyboard_, kBodyFont, LV_PART_ITEMS);
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(keyboard_, OnKeyboardReadyOrCancel, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard_, OnKeyboardReadyOrCancel, LV_EVENT_CANCEL, nullptr);
}

void OnScreenKeyboard::AttachTo(lv_obj_t* textarea) {
    lv_obj_add_event_cb(textarea, OnTextAreaFocused, LV_EVENT_FOCUSED, keyboard_);
}

}  // namespace homedeck
