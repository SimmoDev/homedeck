#pragma once

#include "lvgl.h"

namespace homedeck {

// A reusable on-screen keyboard for touch text entry - wraps a single
// lv_keyboard, attached on demand to whichever lv_textarea the user taps
// on the screen that owns this instance. Knows nothing about what the
// text is for (Wi-Fi credentials, a search box, a future module's API
// token, etc.) - screens attach their own textareas via AttachTo().
// Follows this project's per-screen controller pattern (see ADR-0004):
// one instance per screen, owned by that screen's controller.
class OnScreenKeyboard {
public:
    explicit OnScreenKeyboard(lv_obj_t* parent);

    // Wires textarea so tapping it shows this keyboard, targeted at it;
    // submitting or cancelling hides the keyboard again. Safe to call for
    // multiple textareas on the same screen - they all share this one
    // keyboard instance.
    void AttachTo(lv_obj_t* textarea);

private:
    lv_obj_t* keyboard_;
};

}  // namespace homedeck
