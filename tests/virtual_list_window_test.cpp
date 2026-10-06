#include "ui/virtual_list_window.h"

#include <gtest/gtest.h>

namespace {

using homedeck::ComputeVisibleRows;
using homedeck::MaxVisibleRows;
using homedeck::VisibleRows;

constexpr int32_t kPitch = 100;
constexpr int32_t kView = 500;

TEST(VirtualListWindowTest, AnEmptyListHasNoVisibleRows) {
    VisibleRows rows = ComputeVisibleRows(0, kView, kPitch, 0, 1);
    EXPECT_EQ(rows.count, 0u);
}

TEST(VirtualListWindowTest, AListTopAlignedWithTheViewShowsTheViewsWorthOfRowsPlusOverscan) {
    // Rows 0..4 fill 500 px exactly; one overscan row below.
    VisibleRows rows = ComputeVisibleRows(0, kView, kPitch, 1000, 1);
    EXPECT_EQ(rows.first, 0u);
    EXPECT_EQ(rows.count, 6u);
}

TEST(VirtualListWindowTest, ScrollingUpPastARowMovesTheWindowDown) {
    // List top at -250: rows 2..7 have a pixel on screen (row 2 spans
    // -50..49, row 7 starts at 450), so one overscan row each side gives 1..8.
    VisibleRows rows = ComputeVisibleRows(-250, kView, kPitch, 1000, 1);
    EXPECT_EQ(rows.first, 1u);
    EXPECT_EQ(rows.first + rows.count - 1, 8u);
}

TEST(VirtualListWindowTest, ARowPartlyAboveTheViewIsStillIncluded) {
    // List top at -1: row 0 has 99 visible pixels.
    VisibleRows rows = ComputeVisibleRows(-1, kView, kPitch, 1000, 0);
    EXPECT_EQ(rows.first, 0u);
}

TEST(VirtualListWindowTest, TheWindowIsClampedToTheListsLastRow) {
    VisibleRows rows = ComputeVisibleRows(-950, kView, kPitch, 10, 2);
    EXPECT_EQ(rows.first + rows.count, 10u);
}

TEST(VirtualListWindowTest, AListFarBelowTheViewStillBindsItsFirstRows) {
    // Nothing is on screen, so only the clamped first row is bound.
    VisibleRows rows = ComputeVisibleRows(5000, kView, kPitch, 1000, 2);
    EXPECT_EQ(rows.first, 0u);
    EXPECT_EQ(rows.count, 1u);
}

TEST(VirtualListWindowTest, AListScrolledFarPastItsEndBindsItsLastRows) {
    VisibleRows rows = ComputeVisibleRows(-5000000, kView, kPitch, 20, 1);
    EXPECT_GT(rows.count, 0u);
    EXPECT_EQ(rows.first + rows.count, 20u);
}

TEST(VirtualListWindowTest, TenThousandRowsStillBindOnlyAViewsWorth) {
    VisibleRows rows = ComputeVisibleRows(-123456, kView, kPitch, 10000, 2);
    EXPECT_LE(rows.count, MaxVisibleRows(kView, kPitch, 2));
}

TEST(VirtualListWindowTest, TheReturnedWindowNeverExceedsThePoolSizeAtAnyScrollOffset) {
    const size_t pool = MaxVisibleRows(kView, kPitch, 1);
    for (int32_t top = 300; top > -3000; top -= 7) {
        EXPECT_LE(ComputeVisibleRows(top, kView, kPitch, 100, 1).count, pool) << "list_top=" << top;
    }
}

TEST(VirtualListWindowTest, DegenerateDimensionsYieldNoRows) {
    EXPECT_EQ(ComputeVisibleRows(0, 0, kPitch, 10, 1).count, 0u);
    EXPECT_EQ(ComputeVisibleRows(0, kView, 0, 10, 1).count, 0u);
    EXPECT_EQ(MaxVisibleRows(kView, 0, 1), 0u);
}

}  // namespace
