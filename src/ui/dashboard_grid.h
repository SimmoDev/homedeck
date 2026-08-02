#pragma once

#include "lvgl.h"
#include "ui/grid_occupancy.h"
#include "ui/widget.h"

#include <vector>

namespace homedeck {

// The dashboard's widget host - see docs/architecture/dashboard.md#widget-system
// and ADR-0008's "Dashboard layout model" decision (a fixed grid, exact
// dimensions left as an M2 implementation detail). kColumns is a
// genuinely arbitrary starting point, not a considered choice - there's
// no real widget catalog yet to size against. Rows have no fixed cap
// (no paging concept exists), so they grow on demand as widgets are
// added - the container stays scrollable for when that content exceeds
// the visible screen.
//
// Rows are a fixed height, not sized to content - matched (see
// dashboard_grid.cpp's kRowHeight) to the Tab5's confirmed 720px panel
// width (see docs/architecture/hardware.md#display-and-touch), minus
// this grid's own padding and inter-cell gaps, divided across kColumns,
// so a 1x1 cell is square rather than however tall its own label happens
// to be. This only holds exactly if the grid's actual rendered width,
// padding, and gap match those assumptions - true on both targets today
// (the padding/gap values are set explicitly, not left to a default),
// but worth re-checking if any of them changes.
//
// Widgets can span multiple columns and rows (see Widget::ColumnSpan/
// RowSpan) - see GridOccupancy::FindPlacement() for the placement
// strategy and GridOccupancy::ClampSpan() for how an out-of-range span
// is handled.
class DashboardGrid {
public:
    static constexpr int kColumns = 4;

    explicit DashboardGrid(lv_obj_t* parent);

    // Widgets construct themselves as children of this container (the
    // same "constructor takes parent" pattern StatusBar uses) - AddWidget
    // only positions an already-created widget into the next free cells,
    // no reparenting involved.
    lv_obj_t* Container() const { return grid_; }

    void AddWidget(Widget& widget);

private:
    // Grows row_dsc_ to include row `row` if it doesn't already,
    // re-pointing LVGL at the (possibly reallocated) row template buffer
    // - see dashboard_grid.cpp for why that's required. Called with the
    // same row index passed to occupancy_'s own growth, from the same
    // call site (AddWidget), so the two stay in lockstep without one
    // owning the other.
    void EnsureRowExists(int row);

    lv_obj_t* grid_;
    // N kRowHeight entries (one per declared row) followed by one
    // LV_GRID_TEMPLATE_LAST terminator.
    std::vector<int32_t> row_dsc_;
    GridOccupancy<kColumns> occupancy_;
};

}  // namespace homedeck
