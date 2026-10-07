#include "ui/screens/kodi_busy_hint.h"

namespace homedeck {

lv_obj_t* CreateKodiBusyHint(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "Kodi is busy loading a list. Controls may not respond until it finishes.");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_hidden(label, true);
    lv_obj_move_to_index(label, 0);
    return label;
}

}  // namespace homedeck
