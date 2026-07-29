#pragma once

#include <bitset>
#include <vector>

namespace homedeck {

// The pure grid-packing half of DashboardGrid (src/ui/dashboard_grid.h) -
// which cells a footprint occupies, and where the next one fits. Kept
// LVGL-free so it's unit-testable directly (see tests/grid_occupancy_test.cpp),
// unlike DashboardGrid itself, which also owns the LVGL row descriptor
// array and must grow it in lockstep with this class's own row growth -
// both triggered from the same call site (DashboardGrid::AddWidget),
// with the same row index, rather than one owning the other.
template <int Columns>
class GridOccupancy {
public:
    struct Placement {
        int row;
        int col;
    };

    GridOccupancy() : occupancy_(1) {}

    // Grows to include `row` if it doesn't already exist - a no-op
    // otherwise. Called directly by MarkOccupied(); exposed separately
    // only so DashboardGrid can query RowCount() after a placement
    // without having to re-derive it from Fits()'s own lazy growth.
    void EnsureRowExists(int row) {
        while (static_cast<int>(occupancy_.size()) <= row) {
            occupancy_.emplace_back();
        }
    }

    bool Fits(int row, int col, int col_span, int row_span) const {
        if (col + col_span > Columns) return false;
        for (int r = row; r < row + row_span; r++) {
            if (r >= static_cast<int>(occupancy_.size())) continue;  // not-yet-declared rows are free
            for (int c = col; c < col + col_span; c++) {
                if (occupancy_[r][c]) return false;
            }
        }
        return true;
    }

    void MarkOccupied(int row, int col, int col_span, int row_span) {
        EnsureRowExists(row + row_span - 1);
        for (int r = row; r < row + row_span; r++) {
            for (int c = col; c < col + col_span; c++) {
                occupancy_[r].set(c);
            }
        }
    }

    // First-fit scan: top-to-bottom, left-to-right, the first position a
    // footprint of this size fits without overlapping an already-placed
    // widget - the same default ("sparse") auto-placement CSS Grid uses,
    // not the denser packing that would backfill gaps a later widget
    // could still fit into.
    Placement FindPlacement(int col_span, int row_span) const {
        int row = 0;
        int col = 0;
        while (!Fits(row, col, col_span, row_span)) {
            col++;
            if (col >= Columns) {
                col = 0;
                row++;
            }
        }
        return {row, col};
    }

    int RowCount() const { return static_cast<int>(occupancy_.size()); }

private:
    // One entry per declared row; occupancy_[r][c] is true once some
    // footprint's span covers (r, c).
    std::vector<std::bitset<Columns>> occupancy_;
};

}  // namespace homedeck
