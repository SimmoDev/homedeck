#include "ui/remote_button.h"

namespace homedeck {

namespace {

// kRemoteButtonHeight's own decomposition (27px line height * 2 + this
// value * 2) - kept as a named constant here (not just inline in both
// places) since CreateRemoteButton's label height below needs the same
// number pad_ver was built from, to stay in sync if either changes.
constexpr int32_t kButtonVerticalPad = 28;

}  // namespace

lv_obj_t* CreateRemoteButton(lv_obj_t* parent, const std::string& label_text, int32_t width) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, width);
    lv_obj_set_height(button, kRemoteButtonHeight);
    lv_obj_set_style_pad_ver(button, kButtonVerticalPad, 0);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, label_text.c_str());
    // A label with no width constraint sizes to its own content and, once
    // that's wider than the button (routine at the 31%-width DevicesScreen
    // uses for its command grid - "Volume Down"/"Direction Right" and the
    // like don't fit one line at that width), gets clipped at the
    // button's edge instead of wrapping - there's nothing for
    // LV_LABEL_LONG_WRAP (the default long mode already) to wrap against
    // without an explicit width. Matching the button's own width fixes
    // that; center-aligning the text keeps a wrapped two-line label
    // looking the same as a single-line one instead of defaulting left.
    lv_obj_set_width(label, LV_PCT(100));
    // kRemoteButtonHeight budgets exactly two lines (see its own comment);
    // a third line (a long SplitCamelCase()'d multi-word command label at
    // the 31%-width grid) would otherwise overflow the button and clip
    // silently with no indication anything's missing. Giving the label an
    // explicit height matching that two-line budget lets LONG_DOT replace
    // an overflowing last line with an ellipsis instead - LVGL only
    // truncates a label against a fixed height, not just a fixed width.
    lv_obj_set_height(label, kRemoteButtonHeight - 2 * kButtonVerticalPad);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    // lv_button_create() doesn't give its child a centering layout of its
    // own (no flex/grid set on the button by LVGL's source, default
    // theme included), so the label sits at its default top-left
    // position rather than centering vertically - invisible
    // while every button's height auto-fit its own one-line label (no
    // vertical slack to reveal it), became visible once DevicesScreen's
    // grid started giving every button the same fixed height regardless
    // of its own label's line count.
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    return button;
}

}  // namespace homedeck
