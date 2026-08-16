#pragma once

#include "lvgl.h"

#include <string>

namespace homedeck {

// A large, labeled button - the common shape every Harmony remote-control
// button builds on (ActivitiesScreen's activities, DevicesScreen's
// devices and their commands - see docs/roadmap.md's M3
// Activities/Devices/Remote control items). Sized deliberately larger
// than the theme's own default button padding - see ActivitiesScreen's
// own original comment on why: this is the remote control, the primary
// reason CLAUDE.md calls for "large touch targets" in the Touch UI at
// all. Factored out once Devices needed the identical styling
// ActivitiesScreen's own buttons already established, rather than a
// third independent copy. `width` defaults to full-width (one button per
// row - activity/device lists, where labels are long and unpredictable);
// DevicesScreen's own command grid passes a narrower width instead (see
// its own comment on why 3 per row).
lv_obj_t* CreateRemoteButton(lv_obj_t* parent, const std::string& label_text, int32_t width = LV_PCT(100));

}  // namespace homedeck
