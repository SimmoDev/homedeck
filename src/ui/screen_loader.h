#pragma once

#include "lvgl.h"
#include "ui/lazy_load.h"

#include <cstddef>
#include <functional>

namespace homedeck {

// Runs a screen's `load` callback (a Kodi library request) when the screen
// is first shown and after each reconnect while it is showing, instead of
// at construction. AppCore builds every screen at start-up, so a request
// issued from a constructor would run for screens the user never opens.
// The decisions are LazyLoad's; this class only connects them to
// LV_EVENT_SCREEN_LOAD_START.
class ScreenLoader {
public:
    using ConnectedFn = std::function<bool()>;
    using LoadFn = std::function<void()>;

    ScreenLoader(lv_obj_t* screen, ConnectedFn connected, LoadFn load);

    ScreenLoader(const ScreenLoader&) = delete;
    ScreenLoader& operator=(const ScreenLoader&) = delete;

    // Call when the source has just (re)connected.
    void OnConnected();

    // Call with each load's result. A timed-out listing with no rows is
    // not data worth keeping, so showing the screen again retries it.
    void OnLoaded(size_t item_count, bool truncated) {
        if (truncated && item_count == 0) {
            lazy_load_.MarkStale();
        }
    }

private:
    static void OnScreenLoadStart(lv_event_t* e);

    lv_obj_t* screen_;
    ConnectedFn connected_;
    LoadFn load_;
    LazyLoad lazy_load_;
};

}  // namespace homedeck
