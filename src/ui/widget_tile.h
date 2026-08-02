#pragma once

#include "lvgl.h"

namespace homedeck {

// The common dashboard-tile root every Widget implementation builds on:
// 8px padding, non-scrollable, column flex, centered on both axes so
// stacked content stays centered regardless of the cell's exact size
// rather than needing hand-tuned pixel offsets tied to today's row
// height. Factored out once ClockWidget/NetworkStatusWidget/
// WeatherWidget/NotificationWidget had all independently repeated the
// identical setup.
lv_obj_t* CreateWidgetTileRoot(lv_obj_t* parent);

}  // namespace homedeck
