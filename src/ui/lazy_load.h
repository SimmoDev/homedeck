#pragma once

namespace homedeck {

// Decides when a screen whose data is expensive to fetch should request
// it: only once the screen is showing and its source is connected, and
// again after each reconnect. Pure so the decisions are host-tested;
// ScreenLoader (screen_loader.h) supplies the LVGL events.
class LazyLoad {
public:
    // The source (re)connected, so whatever the screen holds is stale.
    // True if the screen is showing and should load now; otherwise the
    // load waits for OnShown().
    bool OnConnected(bool screen_active) {
        stale_ = true;
        return TakeIf(screen_active);
    }

    // The load returned nothing usable (a timed-out listing), so the next
    // OnShown() must request it again rather than keep the empty result
    // until the next reconnect.
    void MarkStale() { stale_ = true; }

    // The screen is about to be shown. True if it holds no current data
    // and the source is connected.
    bool OnShown(bool connected) { return TakeIf(connected); }

private:
    bool TakeIf(bool ready) {
        if (!stale_ || !ready) {
            return false;
        }
        stale_ = false;
        return true;
    }

    bool stale_ = true;
};

}  // namespace homedeck
