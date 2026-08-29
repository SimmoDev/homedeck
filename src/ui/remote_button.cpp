#include "ui/remote_button.h"

namespace homedeck {

namespace {

// The top/bottom padding inside a remote button - the other half of
// kRemoteButtonHeight's decomposition (27px line height * 2 + this value
// * 2), named so the two stay legibly in sync. A label taller than the
// resulting content area (a rare third wrapped line) overflows into this
// padding rather than being clipped.
constexpr int32_t kButtonVerticalPad = 28;

}  // namespace

lv_obj_t* CreateRemoteButton(lv_obj_t* parent, const std::string& label_text, int32_t width) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, width);
    lv_obj_set_height(button, kRemoteButtonHeight);
    lv_obj_set_style_pad_ver(button, kButtonVerticalPad, 0);
    // A flex column that centres its child on both axes - this is what
    // vertically centres the label regardless of how many lines it wraps
    // to. lv_button_create() gives its child no layout of its own, so a
    // one-line label in a fixed-height button (or a fixed-height label
    // box) would otherwise sit at the top: the wrapped two-line labels
    // DevicesScreen's 31%-width command grid produces would look centred
    // while every one-line label read as top-aligned against them.
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, label_text.c_str());
    // Width-constrained so a label wider than the button wraps instead of
    // clipping at the button's edge (routine at the 31%-width command
    // grid - "Volume Down"/"Direction Right" don't fit one line there);
    // text centred so a wrapped two-line label reads the same as a
    // one-line one. Content height, so the flex centring above applies -
    // kRemoteButtonHeight still budgets ~two lines of vertical room
    // (see its own comment); a rare third line overflows into the
    // button's own padding rather than being truncated.
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    return button;
}

void SetTransportGlyph(lv_obj_t* button, bool pointing_left) {
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label == nullptr) {
        return;
    }
    lv_label_set_text(label, LV_SYMBOL_PLAY LV_SYMBOL_PLAY);
    // Close the gap between the two triangles so they read as one FF/RW
    // glyph, without overlapping them into a single blob - tuned by eye
    // at the body font size (see remote_button.h).
    lv_obj_set_style_text_letter_space(label, -5, 0);
    if (pointing_left) {
        lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_rotation(label, 1800, 0);  // 0.1deg units -> 180deg
    }
}

lv_obj_t* CreateNavChromeButton(lv_obj_t* parent, const char* label_text) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_style_min_width(button, kMinNavTouchTarget, 0);
    lv_obj_set_style_min_height(button, kMinNavTouchTarget, 0);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, label_text);
    // Same reason CreateRemoteButton aligns its own label above -
    // lv_button_create() gives its child no centering layout, so once
    // min-sizing makes the button taller/wider than the label it would
    // otherwise sit in the top-left corner.
    lv_obj_center(label);

    return button;
}

}  // namespace homedeck
