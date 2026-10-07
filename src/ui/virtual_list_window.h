#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace homedeck {

// The half-open row range [first, first + count) a VirtualList must have
// bound to a button.
struct VisibleRows {
    size_t first = 0;
    size_t count = 0;
};

// Which rows of a `total`-row list of equal-height rows (`pitch` pixels
// from one row's top edge to the next) intersect a view `view_height`
// pixels tall whose top edge is at y = 0, when the list's first row starts
// at `list_top` (negative once the list has been scrolled up past it).
// `overscan` extra rows are included above and below so a scroll gesture
// finds its next rows already bound. Pure so the arithmetic is host-tested;
// VirtualList (virtual_list.h) supplies the LVGL coordinates.
inline VisibleRows ComputeVisibleRows(int32_t list_top, int32_t view_height, int32_t pitch, size_t total,
                                      size_t overscan) {
    if (total == 0 || pitch <= 0 || view_height <= 0) {
        return {};
    }
    // Floor division towards negative infinity: C++ truncates towards
    // zero, which would round a row that is partly above the view the
    // wrong way.
    auto floor_div = [](int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    const int64_t first_on_screen = floor_div(-static_cast<int64_t>(list_top), pitch);
    const int64_t last_on_screen = floor_div(static_cast<int64_t>(view_height) - 1 - list_top, pitch);

    const int64_t last_row = static_cast<int64_t>(total) - 1;
    const int64_t first = std::clamp<int64_t>(first_on_screen - static_cast<int64_t>(overscan), 0, last_row);
    const int64_t last = std::clamp<int64_t>(last_on_screen + static_cast<int64_t>(overscan), 0, last_row);
    return {static_cast<size_t>(first), static_cast<size_t>(last - first + 1)};
}

// The row in `window` that pool slot `slot` shows, given that row r uses
// slot r % pool_size, or `kNoRow` when the slot has none (it is beyond the
// pool, or its row is outside the window). `pool_size` must not exceed the
// list's row count, so every slot below it is used when the whole list is
// visible.
inline constexpr size_t kNoRow = static_cast<size_t>(-1);

inline size_t RowForSlot(size_t slot, size_t pool_size, VisibleRows window) {
    if (pool_size == 0 || slot >= pool_size || window.count == 0) {
        return kNoRow;
    }
    const size_t offset = (slot + pool_size - window.first % pool_size) % pool_size;
    return offset < window.count ? window.first + offset : kNoRow;
}

// The most rows ComputeVisibleRows() can return for these dimensions - what
// a VirtualList's button pool has to hold. The +2 covers a row partly off
// both the top and the bottom edge at once.
inline size_t MaxVisibleRows(int32_t view_height, int32_t pitch, size_t overscan) {
    if (pitch <= 0 || view_height <= 0) {
        return 0;
    }
    return static_cast<size_t>((view_height + pitch - 1) / pitch) + 2 + 2 * overscan;
}

}  // namespace homedeck
