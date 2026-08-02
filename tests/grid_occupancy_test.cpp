#include "ui/grid_occupancy.h"

#include <gtest/gtest.h>

namespace {

using FourColumnGrid = homedeck::GridOccupancy<4>;

}  // namespace

TEST(GridOccupancyTest, FirstWidgetPlacesAtOriginOnAFreshGrid) {
    FourColumnGrid grid;
    auto placement = grid.FindPlacement(1, 1);
    EXPECT_EQ(placement.row, 0);
    EXPECT_EQ(placement.col, 0);
}

TEST(GridOccupancyTest, SecondOneByOneWidgetPlacesNextToTheFirst) {
    FourColumnGrid grid;
    grid.MarkOccupied(0, 0, 1, 1);
    auto placement = grid.FindPlacement(1, 1);
    EXPECT_EQ(placement.row, 0);
    EXPECT_EQ(placement.col, 1);
}

TEST(GridOccupancyTest, WrapsToTheNextRowWhenTheCurrentOneIsFull) {
    FourColumnGrid grid;
    for (int col = 0; col < 4; col++) {
        grid.MarkOccupied(0, col, 1, 1);
    }
    auto placement = grid.FindPlacement(1, 1);
    EXPECT_EQ(placement.row, 1);
    EXPECT_EQ(placement.col, 0);
}

TEST(GridOccupancyTest, WideWidgetSkipsAColumnThatCannotFitItsFullSpan) {
    FourColumnGrid grid;
    // Columns 0-2 occupied, leaving only column 3 free on row 0 - a
    // 2-column-span widget can't fit there (would need columns 3-4, and
    // there is no column 4), so it must wrap to row 1 entirely, not
    // partially overlap column 3.
    grid.MarkOccupied(0, 0, 3, 1);
    auto placement = grid.FindPlacement(2, 1);
    EXPECT_EQ(placement.row, 1);
    EXPECT_EQ(placement.col, 0);
}

TEST(GridOccupancyTest, MultiRowSpanBlocksBothRowsItCovers) {
    FourColumnGrid grid;
    grid.MarkOccupied(0, 0, 1, 2);  // occupies (0,0) and (1,0)

    EXPECT_FALSE(grid.Fits(0, 0, 1, 1));
    EXPECT_FALSE(grid.Fits(1, 0, 1, 1));
    EXPECT_TRUE(grid.Fits(0, 1, 1, 1));
    EXPECT_TRUE(grid.Fits(2, 0, 1, 1));
}

TEST(GridOccupancyTest, FitsTreatsNotYetDeclaredRowsAsFree) {
    FourColumnGrid grid;
    // Nothing has ever touched row 5 - it must still read as available,
    // not rejected just because it hasn't been grown into yet.
    EXPECT_TRUE(grid.Fits(5, 0, 1, 1));
}

TEST(GridOccupancyTest, FitsRejectsASpanThatWouldExceedTheColumnCount) {
    FourColumnGrid grid;
    EXPECT_FALSE(grid.Fits(0, 3, 2, 1));  // columns 3-4, but there are only 4 columns (0-3)
    EXPECT_TRUE(grid.Fits(0, 2, 2, 1));   // columns 2-3, exactly fits
}

TEST(GridOccupancyTest, RowCountGrowsToCoverMarkedRowsOnly) {
    FourColumnGrid grid;
    EXPECT_EQ(grid.RowCount(), 1);
    grid.MarkOccupied(2, 0, 1, 1);
    EXPECT_EQ(grid.RowCount(), 3);
}

TEST(GridOccupancyTest, EnsureRowExistsIsANoOpWhenTheRowAlreadyExists) {
    FourColumnGrid grid;
    grid.MarkOccupied(2, 0, 1, 1);
    ASSERT_EQ(grid.RowCount(), 3);
    grid.EnsureRowExists(1);
    EXPECT_EQ(grid.RowCount(), 3);
}

TEST(GridOccupancyTest, ClampSpanFloorsColumnAndRowSpanBelowOne) {
    auto span = FourColumnGrid::ClampSpan(0, -3);
    EXPECT_EQ(span.col_span, 1);
    EXPECT_EQ(span.row_span, 1);
}

TEST(GridOccupancyTest, ClampSpanCapsColumnSpanAtTheColumnCount) {
    auto span = FourColumnGrid::ClampSpan(6, 1);
    EXPECT_EQ(span.col_span, 4);
}

TEST(GridOccupancyTest, ClampSpanCapsRowSpanAtTheMaxRowSpanCeiling) {
    auto span = FourColumnGrid::ClampSpan(1, 1000000);
    EXPECT_EQ(span.row_span, FourColumnGrid::kMaxRowSpan);
}

TEST(GridOccupancyTest, ClampSpanLeavesAlreadyValidSpansUnchanged) {
    auto span = FourColumnGrid::ClampSpan(2, 3);
    EXPECT_EQ(span.col_span, 2);
    EXPECT_EQ(span.row_span, 3);
}

TEST(GridOccupancyTest, FindPlacementTerminatesForAColumnSpanWiderThanTheGrid) {
    // Regression: an unclamped col_span > Columns previously made Fits()
    // reject every column at every row, spinning this call forever.
    FourColumnGrid grid;
    auto placement = grid.FindPlacement(6, 1);
    EXPECT_EQ(placement.row, 0);
    EXPECT_EQ(placement.col, 0);
}

TEST(GridOccupancyTest, FindPlacementClampsAnExcessiveRowSpanBeforeSearching) {
    // Regression: row_span previously had no upper bound anywhere in
    // this class, so Fits()'s row loop - and a caller's subsequent
    // row-descriptor growth from the same unclamped value - scaled with
    // whatever a misbehaving Widget::RowSpan() override returned.
    FourColumnGrid grid;
    auto placement = grid.FindPlacement(1, 1000000);
    EXPECT_EQ(placement.row, 0);
    EXPECT_EQ(placement.col, 0);
}
