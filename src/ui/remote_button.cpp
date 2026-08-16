#include "ui/remote_button.h"

namespace homedeck {

lv_obj_t* CreateRemoteButton(lv_obj_t* parent, const std::string& label_text) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_style_pad_ver(button, 28, 0);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, label_text.c_str());

    return button;
}

}  // namespace homedeck
