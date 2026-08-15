#pragma once

#include "lvgl.h"

namespace homedeck {

// The standard interface modules (and Core itself, for things like
// weather) contribute dashboard content through - see
// docs/architecture/dashboard.md#widget-system and ADR-0008. The
// dashboard hosts widgets without knowing their concrete type; a module
// can ship a new widget without any Core change.
//
// Deliberately minimal beyond span: no live/cached/offline freshness
// reporting yet - ADR-0008 names it as an open implementation detail,
// and there's no real widget yet (weather, the first one, is a separate
// follow-up pass) to validate what shape it actually needs.
class Widget {
public:
    virtual ~Widget() = default;

    // Must be a child of the DashboardGrid::Container() the widget was
    // constructed against - see dashboard_grid.h.
    virtual lv_obj_t* Root() const = 0;

    // Footprint in grid cells - see DashboardGrid. Defaults to 1x1;
    // override to occupy more. ColumnSpan() should be between 1 and
    // DashboardGrid::kColumns; RowSpan() should be at least 1, with no
    // practical ceiling (rows grow to fit). DashboardGrid::AddWidget()
    // clamps both via GridOccupancy::ClampSpan() (including a defensive
    // upper bound on RowSpan(), see grid_occupancy.h's kMaxRowSpan)
    // rather than assuming every implementation gets this right.
    virtual int ColumnSpan() const { return 1; }
    virtual int RowSpan() const { return 1; }

    // Called when the widget's tile is tapped - see DashboardGrid::AddWidget(),
    // which wires every widget's Root() to this uniformly. No-op default:
    // most widgets are glanceable-only, nothing to navigate to. HarmonyWidget
    // (src/ui/harmony_widget.h) is the first override - deferred until a
    // real widget needed tap-for-detail, per this project's "no premature
    // abstraction" discipline (see roadmap.md's M2 Widget framework item).
    virtual void OnTap() {}
};

}  // namespace homedeck
