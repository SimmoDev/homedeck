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

// Minimum width/height for navigation chrome that's deliberately lighter-
// weight than a full remote-control button (the home affordance,
// DevicesScreen's own "Devices" nav button on ActivitiesScreen - see each
// one's own comment on why they're visually secondary) - CLAUDE.md's
// "large touch targets" requirement isn't scoped to primary buttons only,
// but LVGL's default theme button padding auto-fits to content with no
// guaranteed minimum, so a short label/icon-only button can end up
// smaller than this without it being a deliberate choice either way.
// 88 = Android's own 48dp minimum touch-target guideline converted to
// this panel's actual ~294 PPI (48 * 294 / 160, dp being defined against
// a 160dpi baseline) rather than treated as a raw pixel count - the same
// dp/PPI conversion dashboard.md's status-bar font-size reasoning already
// does. A raw 48px here would be ~4.1mm at this density, well under every
// platform's physical touch-target floor (Android 48dp/~7.6mm, Apple
// 44pt/~6.9mm, WCAG 2.5.5/~9mm).
constexpr int32_t kMinNavTouchTarget = 88;

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
