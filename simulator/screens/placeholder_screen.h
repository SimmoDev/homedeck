#pragma once

#include "lvgl.h"
#include "ui/navigation.h"

// A deliberately minimal, throwaway second screen - exists only to prove
// the Navigation manager + persistent home affordance mechanism actually
// works (see docs/roadmap.md's M1 "Persistent home affordance" item).
// Not real product UI - simulator-only, like the earlier
// HeartbeatScreenController it follows the same pattern of. Replaced once
// a genuine second screen exists (an M2 settings screen, or the first M3
// module screen).
class PlaceholderScreen {
public:
    explicit PlaceholderScreen(homedeck::Navigation& navigation);

    lv_obj_t* Root() const { return root_; }

private:
    lv_obj_t* root_;
};
