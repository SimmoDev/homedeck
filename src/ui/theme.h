#pragma once

#include "lvgl.h"

namespace homedeck {

// The default body/label font used throughout the Touch UI - one shared
// constant instead of every widget/screen either repeating the
// &lv_font_montserrat_24 literal or independently redefining its own
// local constant for the same value. Montserrat 24 is the closest
// LVGL-available discrete size to Android's status-bar text on this
// panel's ~294 PPI (see dashboard.md#status for the math). ClockWidget's
// large time display is a deliberately different, larger font
// (lv_font_montserrat_48) and stays its own literal - it isn't reused
// anywhere else.
constexpr const lv_font_t* kBodyFont = &lv_font_montserrat_24;

}  // namespace homedeck
