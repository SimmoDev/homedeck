#pragma once

#include "core/event_bus.h"
#include "core/network_status_monitor.h"
#include "lvgl.h"
#include "platform/network_status.h"
#include "ui/widget.h"

namespace homedeck {

// Network status (Core) - see docs/architecture/dashboard.md's
// compact/detailed split: the status bar's Wi-Fi icon is presence-only,
// this widget adds the detail an icon can't show (SSID, IP). Refreshes
// on WifiConnectivityChangedEvent rather than the clock tick, same
// reasoning as StatusBar's own wifi_subscription_ - connectivity is a
// discrete, rare transition, not something a 1Hz poll needs to catch.
// Known gap: NetworkStatusMonitor only publishes on a connected/
// disconnected transition, so an IP renewal that doesn't pass through a
// disconnect (a DHCP lease change while still associated) won't refresh
// this widget's IP label. Not observed in practice and not worth a
// second event/poll path until it is.
class NetworkStatusWidget : public Widget {
public:
    explicit NetworkStatusWidget(lv_obj_t* parent, EventBus& event_bus, NetworkStatus& network_status);

    lv_obj_t* Root() const override { return root_; }
    // 2 columns, same as ClockWidget - a 1x1 (170px) cell is too narrow
    // for a full IP address ("255.255.255.255") at Montserrat 24 without
    // truncating, and this widget's entire purpose is showing SSID/IP in
    // full, not just an icon.
    int ColumnSpan() const override { return 2; }

private:
    lv_obj_t* root_;
    lv_obj_t* state_label_;
    lv_obj_t* ssid_label_;
    lv_obj_t* ip_label_;
    EventBus::ScopedSubscription wifi_subscription_;
};

}  // namespace homedeck
