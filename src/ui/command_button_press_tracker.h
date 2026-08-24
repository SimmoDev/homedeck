#pragma once

#include <unordered_set>

namespace homedeck {

// Pure long-press/scroll-vs-tap decision logic behind DevicesScreen's
// command buttons - extracted so it's host-testable without LVGL, same
// precedent as ActivityStartTracker/text_format.h. Mirrors DevicesScreen's
// own three LVGL event handlers (OnCommandButtonLongPressed/
// LongPressRepeat/Released) one-to-one; this class only decides which of
// Press/Hold/Release to send (if any) and tracks which buttons are
// currently mid-long-press - it never touches LVGL, sends the actual
// command, or reads input-device state itself. `button` is treated as an
// opaque identity, not a real LVGL widget - DevicesScreen passes its own
// lv_obj_t* through as a raw pointer, so this header carries no LVGL
// dependency at all.
class CommandButtonPressTracker {
public:
    enum class Action { kNone, kPress, kHold, kRelease, kPressAndRelease };

    // LV_EVENT_LONG_PRESSED - marks `button` mid-long-press and always
    // returns kPress.
    Action OnLongPressed(const void* button);
    // LV_EVENT_LONG_PRESSED_REPEAT - always returns kHold; per LVGL's own
    // indev.c this only ever fires after LONG_PRESSED already did, so
    // there's no separate "is this button actually mid-long-press" check
    // to make here.
    Action OnLongPressRepeat(const void* button) const;
    // LV_EVENT_RELEASED - `is_scrolling` is the caller's own
    // lv_indev_get_scroll_obj() != nullptr check (this class has no way
    // to ask LVGL itself). Clears `button`'s mid-long-press marking if
    // set - a genuine long press always returns kRelease regardless of
    // is_scrolling (dragging while still holding must not leave the
    // device thinking the button is stuck down); otherwise kNone for a
    // scroll that happened to start on this button, or kPressAndRelease
    // for a plain tap.
    Action OnReleased(const void* button, bool is_scrolling);

    // Called from DevicesScreen::ShowDeviceDetail()'s own rebuild, which
    // deletes every command button - mirrors the original
    // long_press_active_buttons_.clear() call site.
    void Reset();

private:
    std::unordered_set<const void*> long_press_active_buttons_;
};

}  // namespace homedeck
