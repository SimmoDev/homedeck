#include "ui/screens/harmony_screen_chrome.h"

#include "ui/home_affordance.h"
#include "ui/status_bar.h"
#include "ui/theme.h"

namespace homedeck {

HarmonyScreenChrome CreateHarmonyScreenChrome(lv_obj_t* root, const char* title, Navigation& navigation) {
    lv_obj_set_style_text_font(root, kBodyFont, 0);

    lv_obj_t* container = lv_obj_create(root);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(container, LV_ALIGN_TOP_MID, 0, StatusBar::kHeight + 16);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(container, 8, 0);
    lv_obj_set_style_pad_all(container, 16, 0);

    lv_obj_t* title_label = lv_label_create(container);
    lv_label_set_text(title_label, title);

    lv_obj_t* hint_label = lv_label_create(container);
    lv_label_set_text(hint_label, "Not connected to a Harmony Hub yet.");
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint_label, LV_PCT(90));
    lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t* list_container = lv_obj_create(container);
    lv_obj_remove_style_all(list_container);
    lv_obj_set_size(list_container, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list_container, LV_FLEX_FLOW_COLUMN);
    // Deliberate spacing between genuinely large buttons, not a tight list -
    // see each button's own pad_ver comment (remote_button.cpp) for why
    // "large" is the deliberate target here, not a compact default. root
    // (the screen itself) is left scrollable, same as DashboardGrid's own
    // "content can exceed the visible screen" handling, so a longer list
    // still works.
    lv_obj_set_style_pad_row(list_container, 12, 0);

    lv_obj_t* home_button = CreateHomeAffordance(root, navigation);

    return {container, hint_label, list_container, home_button};
}

}  // namespace homedeck
