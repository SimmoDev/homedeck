#pragma once

#include "lvgl.h"

#include <string>

namespace homedeck {

// Every remote-control button is this tall, list buttons (activities,
// devices) and command-grid buttons alike - one consistent height across
// every screen this app renders, rather than list buttons auto-fitting
// their own single-line label while grid buttons use a fixed height.
// 110 = kBodyFont's 27px line height * 2 (room for a two-line label) +
// the 28px top/bottom padding CreateRemoteButton already applies * 2 -
// sized for the command grid's own mixed one-line/two-line labels (e.g.
// "Volume Up" vs "Volume Down" in the same row), which list buttons'
// shorter, more predictable labels don't strictly need but share anyway
// for a consistent look and feel.
constexpr int32_t kRemoteButtonHeight = 110;

// A large, labeled button - the common shape every Harmony remote-control
// button builds on (ActivitiesScreen's activities, DevicesScreen's
// devices and their commands - see docs/roadmap.md's M3
// Activities/Devices/Remote control items). Sized deliberately larger
// than the theme's own default button padding - see ActivitiesScreen's
// own original comment on why: this is the remote control, the primary
// reason CLAUDE.md calls for "large touch targets" in the Touch UI at
// all. Factored out once Devices needed the identical styling
// ActivitiesScreen's own buttons already established, rather than a
// third independent copy.
//
// `width` defaults to full-width (one button per row - activity/device
// lists, where labels are long and unpredictable); DevicesScreen's own
// command grid passes a narrower width instead (see its own comment on
// why 3 per row). Height is always kRemoteButtonHeight - not a
// parameter, since every caller wants the same value.
lv_obj_t* CreateRemoteButton(lv_obj_t* parent, const std::string& label_text, int32_t width = LV_PCT(100));

}  // namespace homedeck
