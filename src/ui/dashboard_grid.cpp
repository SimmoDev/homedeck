#include "ui/dashboard_grid.h"

#include "ui/status_bar.h"

namespace homedeck {

namespace {

constexpr int32_t kColumnTemplate[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                        LV_GRID_TEMPLATE_LAST};
static_assert(sizeof(kColumnTemplate) / sizeof(kColumnTemplate[0]) == DashboardGrid::kColumns + 1,
              "kColumnTemplate must have one LV_GRID_FR(1) entry per column, plus the terminator");

// Confirmed panel width (docs/architecture/hardware.md#display-and-touch)
// minus this grid's own 8px-per-side padding and the 8px gaps between
// columns (3 gaps across kColumns=4 columns), divided across kColumns -
// a fixed row height matching the resulting column width makes a 1x1
// cell square instead of however tall its own content happens to be.
// The gap has to be part of this calculation, not applied on top of it
// separately - it eats into the same available width the column size
// comes from. Only accurate if the grid's actual rendered width,
// padding, and gap match these assumptions (kGridPadding/kCellGap are
// set explicitly below so they do); a real widget catalog may need this
// revisited regardless.
constexpr int32_t kPanelWidth = 720;
constexpr int32_t kGridPadding = 8;
constexpr int32_t kCellGap = 8;
constexpr int32_t kRowHeight =
    (kPanelWidth - 2 * kGridPadding - (DashboardGrid::kColumns - 1) * kCellGap) /
    DashboardGrid::kColumns;

}  // namespace

DashboardGrid::DashboardGrid(lv_obj_t* parent)
    : row_dsc_{kRowHeight, LV_GRID_TEMPLATE_LAST}, occupancy_(1) {
    grid_ = lv_obj_create(parent);
    lv_obj_set_grid_dsc_array(grid_, kColumnTemplate, row_dsc_.data());
    lv_obj_set_size(grid_, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(grid_, LV_ALIGN_TOP_MID, 0, StatusBar::kHeight);
    lv_obj_set_style_pad_all(grid_, kGridPadding, 0);
    lv_obj_set_style_pad_row(grid_, kCellGap, 0);
    lv_obj_set_style_pad_column(grid_, kCellGap, 0);
    lv_obj_set_style_border_width(grid_, 0, 0);
    // Unlike StatusBar (fixed height, never overflows its own bounds),
    // this grid's content genuinely can exceed the visible screen as
    // more widgets are added - rows have no fixed cap - so scrolling
    // stays enabled here. This is the opposite fix from status_bar.cpp,
    // which clears scrolling to prevent it happening *accidentally* from
    // small content overflowing tight padding; this container's
    // scrolling is real and wanted, not a default left in by mistake.
}

void DashboardGrid::EnsureRowExists(int row) {
    if (row < static_cast<int>(occupancy_.size())) return;

    while (static_cast<int>(occupancy_.size()) <= row) {
        occupancy_.emplace_back();
        row_dsc_.back() = kRowHeight;  // overwrite the old terminator with a real row
        row_dsc_.push_back(LV_GRID_TEMPLATE_LAST);  // re-terminate
    }

    // LVGL's grid style property stores this pointer directly rather
    // than copying the array (confirmed via lv_obj_style_gen.c), so
    // growing row_dsc_ - which may reallocate its buffer - requires
    // re-pointing LVGL at wherever the data actually lives now. Skipping
    // this after a reallocation would leave LVGL holding a dangling
    // pointer into freed memory.
    lv_obj_set_grid_dsc_array(grid_, kColumnTemplate, row_dsc_.data());
}

bool DashboardGrid::Fits(int row, int col, int col_span, int row_span) const {
    if (col + col_span > kColumns) return false;
    for (int r = row; r < row + row_span; r++) {
        if (r >= static_cast<int>(occupancy_.size())) continue;  // not-yet-declared rows are free
        for (int c = col; c < col + col_span; c++) {
            if (occupancy_[r][c]) return false;
        }
    }
    return true;
}

void DashboardGrid::MarkOccupied(int row, int col, int col_span, int row_span) {
    EnsureRowExists(row + row_span - 1);
    for (int r = row; r < row + row_span; r++) {
        for (int c = col; c < col + col_span; c++) {
            occupancy_[r].set(c);
        }
    }
}

void DashboardGrid::AddWidget(Widget& widget) {
    int col_span = widget.ColumnSpan();
    int row_span = widget.RowSpan();

    int row = 0;
    int col = 0;
    while (!Fits(row, col, col_span, row_span)) {
        col++;
        if (col >= kColumns) {
            col = 0;
            row++;
        }
    }

    MarkOccupied(row, col, col_span, row_span);
    lv_obj_set_grid_cell(widget.Root(), LV_GRID_ALIGN_STRETCH, col, col_span, LV_GRID_ALIGN_STRETCH,
                          row, row_span);
    // lv_obj_create() is scrollable by default - StatusBar hit a real
    // scrollbar/drag-scroll bug from this exact default (see
    // status_bar.cpp). Cleared centrally here, on the widget's own root,
    // rather than requiring every Widget implementation to remember it -
    // an internally-scrollable widget can still nest its own scrollable
    // child inside Root(), since this only affects Root() itself.
    lv_obj_clear_flag(widget.Root(), LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace homedeck
