#pragma once

#include "lvgl.h"

namespace homedeck {

// A hidden, centered, word-wrapping note a Kodi control screen shows while
// KodiSnapshot::library_busy is set (Kodi answers nothing else while it
// finishes a slow library request). Created as `parent`'s first child, so
// it sits above the controls it explains.
lv_obj_t* CreateKodiBusyHint(lv_obj_t* parent);

}  // namespace homedeck
