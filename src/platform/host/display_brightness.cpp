#include "platform/host/display_brightness.h"

#include <algorithm>

namespace homedeck {

HostDisplayBrightness::HostDisplayBrightness() {
    overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay_);
    lv_obj_set_size(overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_, LV_OPA_TRANSP, 0);
    // Idle dims the screen, it doesn't lock touch - real input still
    // needs to reach the dashboard underneath for Idle->Active to have
    // anything to detect (see ui/lvgl_user_activity_source.h).
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay_, LV_OBJ_FLAG_SCROLLABLE);
}

void HostDisplayBrightness::SetPercent(int percent) {
    int clamped = std::clamp(percent, 0, 100);
    lv_opa_t opa = static_cast<lv_opa_t>((100 - clamped) * LV_OPA_COVER / 100);
    lv_obj_set_style_bg_opa(overlay_, opa, 0);
}

}  // namespace homedeck
