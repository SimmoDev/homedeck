#include "ui/virtual_list.h"

#include "ui/remote_button.h"
#include "ui/virtual_list_window.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace homedeck {

namespace {

constexpr size_t kUnbound = std::numeric_limits<size_t>::max();

// Rows bound beyond each edge of the screen, so a fling finds the next
// rows already bound instead of blank for a frame.
constexpr size_t kOverscan = 2;

constexpr int32_t kRowPitch = kRemoteButtonHeight + VirtualList::kRowGap;

}  // namespace

VirtualList::VirtualList(lv_obj_t* parent, const char* empty_text)
    : list_(lv_obj_create(parent)),
      empty_label_(lv_label_create(parent)),
      loading_label_(lv_label_create(parent)),
      truncated_label_(lv_label_create(parent)),
      screen_(lv_obj_get_screen(parent)) {
    lv_obj_remove_style_all(list_);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_height(list_, 0);
    // Rows are positioned by hand inside a fixed-height box; the screen
    // around it is what scrolls.
    lv_obj_set_scrollable(list_, false);
    lv_obj_set_hidden(list_, true);

    lv_label_set_text(empty_label_, empty_text);
    lv_obj_set_hidden(empty_label_, true);

    lv_obj_set_hidden(truncated_label_, true);

    lv_label_set_text(loading_label_, "Loading...");

    lv_obj_add_event_cb(screen_, OnScreenScrolled, LV_EVENT_SCROLL, this);
}

VirtualList::~VirtualList() {
    // The owning screen deletes its LVGL tree before its members are
    // destroyed, which takes screen_ with it.
    if (lv_obj_is_valid(screen_)) {
        lv_obj_remove_event_cb_with_user_data(screen_, OnScreenScrolled, this);
    }
}

void VirtualList::SetItems(size_t count, LabelFn label_at, SelectFn on_select) {
    count_ = count;
    label_at_ = std::move(label_at);
    on_select_ = std::move(on_select);

    lv_obj_set_hidden(truncated_label_, true);
    lv_obj_set_hidden(loading_label_, true);
    lv_obj_set_hidden(empty_label_, count != 0);
    lv_obj_set_hidden(list_, count == 0);
    lv_obj_set_height(list_, count == 0 ? 0 : static_cast<int32_t>(count) * kRowPitch - kRowGap);

    EnsurePool();
    Rebind(/*force=*/true);
}

void VirtualList::SetTruncated(bool truncated) {
    if (truncated) {
        lv_label_set_text(truncated_label_, count_ != 0
                                                ? ("Only the first " + std::to_string(count_) + " items are shown.").c_str()
                                                : "Kodi took too long to list this.");
        // An incomplete empty list is not an empty library.
        lv_obj_set_hidden(empty_label_, true);
    }
    lv_obj_set_hidden(truncated_label_, !truncated);
}

void VirtualList::Clear() {
    count_ = 0;
    lv_obj_set_hidden(truncated_label_, true);
    label_at_ = nullptr;
    on_select_ = nullptr;
    lv_obj_set_hidden(loading_label_, false);
    lv_obj_set_hidden(empty_label_, true);
    lv_obj_set_hidden(list_, true);
    lv_obj_set_height(list_, 0);
    Rebind(/*force=*/true);
}

void VirtualList::Refresh() { Rebind(/*force=*/false); }

void VirtualList::EnsurePool() {
    const int32_t view_height = lv_display_get_vertical_resolution(nullptr);
    const size_t wanted = std::min(count_, MaxVisibleRows(view_height, kRowPitch, kOverscan));
    while (pool_.size() < wanted) {
        lv_obj_t* button = CreateRemoteButton(list_, "");
        lv_obj_set_hidden(button, true);
        lv_obj_add_event_cb(button, OnRowClicked, LV_EVENT_CLICKED, this);
        pool_.push_back(button);
    }
    // Row r uses slot r % pool_.size(), so a changed pool size moves every
    // row's slot.
    slot_row_.assign(pool_.size(), kUnbound);
}

bool VirtualList::AncestorHidden() const {
    for (const lv_obj_t* object = list_; object != nullptr; object = lv_obj_get_parent(object)) {
        if (lv_obj_is_hidden(object)) {
            return true;
        }
    }
    return false;
}

void VirtualList::Rebind(bool force) {
    // Pool slots beyond the rows to show, or all of them for an empty list.
    VisibleRows rows;
    const size_t pool_size = std::min(pool_.size(), count_);
    if (count_ != 0 && pool_size != 0) {
        // A hidden container has no layout yet, so its position is unknown;
        // bind from the top and let Refresh() correct it once shown.
        int32_t list_top = 0;
        if (!AncestorHidden()) {
            lv_obj_update_layout(list_);
            lv_area_t area;
            lv_obj_get_coords(list_, &area);
            list_top = area.y1;
        }
        rows = ComputeVisibleRows(list_top, lv_display_get_vertical_resolution(nullptr), kRowPitch, count_, kOverscan);
    }

    for (size_t slot = 0; slot < pool_.size(); ++slot) {
        lv_obj_t* button = pool_[slot];
        if (rows.count == 0) {
            lv_obj_set_hidden(button, true);
            slot_row_[slot] = kUnbound;
            continue;
        }
        const size_t row = RowForSlot(slot, pool_size, rows);
        if (row == kNoRow) {
            lv_obj_set_hidden(button, true);
            slot_row_[slot] = kUnbound;
            continue;
        }
        if (force || slot_row_[slot] != row) {
            lv_label_set_text(RemoteButtonLabel(button), label_at_(row).c_str());
            lv_obj_set_pos(button, 0, static_cast<int32_t>(row) * kRowPitch);
            lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(row)));
            slot_row_[slot] = row;
        }
        lv_obj_set_hidden(button, false);
    }
}

void VirtualList::OnScreenScrolled(lv_event_t* e) {
    static_cast<VirtualList*>(lv_event_get_user_data(e))->Rebind(/*force=*/false);
}

void VirtualList::OnRowClicked(lv_event_t* e) {
    auto* self = static_cast<VirtualList*>(lv_event_get_user_data(e));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const auto row = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button)));
    if (self->on_select_ && row < self->count_) {
        // A copy: the handler may replace or clear this list (the Files
        // screen descends into a folder from its own row tap), which would
        // otherwise destroy the callable while it is running.
        const SelectFn on_select = self->on_select_;
        on_select(row);
    }
}

}  // namespace homedeck
