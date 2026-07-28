#include "ui/lvgl_user_activity_source.h"

#include "lvgl.h"

namespace homedeck {

uint32_t LvglUserActivitySource::MillisecondsSinceLastActivity() const {
    // nullptr = minimum inactive time across all registered displays -
    // correct for this project's single-display assumption (same one
    // ui/navigation.h and ui/ui_dispatch.h already make). Reset
    // automatically on any real touch/indev event, regardless of which
    // widget (if any) it hits.
    return lv_display_get_inactive_time(nullptr);
}

}  // namespace homedeck
