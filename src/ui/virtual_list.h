#pragma once

#include "lvgl.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace homedeck {

// A vertical list of large remote buttons (CreateRemoteButton) that keeps
// only the rows near the screen as LVGL objects. A Kodi library can hold
// thousands of entries and one button per entry took ~20 s to build and
// render for a thousand of them on a Tab5, so the browse screens use this
// instead of creating a button per item. Rows are all kRemoteButtonHeight
// tall, `kRowGap` apart.
//
// The screen that contains the list (not the list itself) is what scrolls,
// as everywhere in this UI, so the list watches that screen's LV_EVENT_SCROLL
// and rebinds a small pool of buttons to whichever rows are visible. Row r
// always uses pool slot r % pool size, so a scroll only rebinds the slots
// whose row changed.
//
// Must be destroyed no later than its parent screen, and only touches LVGL
// from the UI thread like everything else in src/ui/.
class VirtualList {
public:
    using LabelFn = std::function<std::string(size_t row)>;
    using SelectFn = std::function<void(size_t row)>;

    static constexpr int32_t kRowGap = 12;

    // `empty_text` is shown instead of rows when SetItems() is given zero
    // items (not after Clear()).
    VirtualList(lv_obj_t* parent, const char* empty_text);
    ~VirtualList();

    VirtualList(const VirtualList&) = delete;
    VirtualList& operator=(const VirtualList&) = delete;

    // `label_at` and `on_select` are called on demand for rows near the
    // screen, so whatever they read must outlive the rows' use - the caller
    // typically keeps the fetched vector as a member and indexes it.
    void SetItems(size_t count, LabelFn label_at, SelectFn on_select);

    // Shows a note below the rows that the list is incomplete (the data
    // source stopped at its size cap or timed out), or, for a list with no
    // rows, that Kodi took too long instead of the empty text. Call after
    // SetItems(), which clears it.
    void SetTruncated(bool truncated);

    // Shows nothing at all: for a list waiting on a reply, where the
    // previous list's rows or an "empty" message would both mislead.
    void Clear();

    // Rebinds the visible rows. Call after showing a container the list
    // is inside: a hidden list has no layout, so its position on screen is
    // unknown until it is shown.
    void Refresh();

    lv_obj_t* Root() const { return list_; }

private:
    void EnsurePool();
    void Rebind(bool force);
    bool AncestorHidden() const;

    static void OnScreenScrolled(lv_event_t* e);
    static void OnRowClicked(lv_event_t* e);

    lv_obj_t* list_;
    lv_obj_t* empty_label_;
    lv_obj_t* truncated_label_;
    lv_obj_t* screen_;

    std::vector<lv_obj_t*> pool_;
    std::vector<size_t> slot_row_;  // the row each pool slot is bound to, or kUnbound

    size_t count_ = 0;
    LabelFn label_at_;
    SelectFn on_select_;
};

}  // namespace homedeck
