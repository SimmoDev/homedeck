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

    // A widget's requested footprint, normalized by ClampSpan() below.
    struct Span {
        int col_span;
        int row_span;
    };

    // No legitimate widget needs anywhere near this many rows (rows
    // grow to fit on demand - see DashboardGrid - so there is no
    // structural need for a ceiling at all). This one exists purely as
    // a defensive bound against a misbehaving Widget::RowSpan()
    // override (wrong units, corrupted state): without it, Fits()'s row
    // loop and the caller's own row-descriptor growth would scale with
    // whatever value is passed in, hanging or exhausting memory on the
    // UI thread.
    static constexpr int kMaxRowSpan = 100;

    // Normalizes a widget-reported footprint into a range this class
    // can place without hanging or mis-placing: col_span clamped to
    // [1, Columns], row_span clamped to [1, kMaxRowSpan]. Centralized
    // here rather than left to each caller so every caller gets the
    // same guarantee and the clamp itself is host-testable - see
    // FindPlacement(), which applies this internally, and
    // DashboardGrid::AddWidget(), which must reuse the same clamped
    // values for MarkOccupied() and the LVGL cell rather than
    // re-deriving them, so placement and occupancy never disagree on
    // the footprint actually used.
    static Span ClampSpan(int col_span, int row_span) {
        if (col_span < 1) col_span = 1;
        if (col_span > Columns) col_span = Columns;
        if (row_span < 1) row_span = 1;
        if (row_span > kMaxRowSpan) row_span = kMaxRowSpan;
        return {col_span, row_span};
    }

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
        // Clamped internally, not just trusted from the caller - an
        // unclamped oversized col_span would make Fits() reject every
        // column at every row below, spinning this loop forever instead
        // of terminating, regardless of which caller passed it in.
        Span clamped = ClampSpan(col_span, row_span);
        col_span = clamped.col_span;
        row_span = clamped.row_span;

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
