#include "ui/remote_button.h"

namespace homedeck {

lv_obj_t* CreateRemoteButton(lv_obj_t* parent, const std::string& label_text, int32_t width) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, width);
    lv_obj_set_style_pad_ver(button, 28, 0);

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
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    return button;
}

}  // namespace homedeck
