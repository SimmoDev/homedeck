#include "ui/screens/dashboard_screen.h"

namespace homedeck {

DashboardScreen::DashboardScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status)
    : root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status),
      grid_(root_) {
    // status_bar_ is constructed (and so added as root_'s child) before
    // grid_ above - FLOATING (see StatusBar's own constructor comment)
    // keeps it from scrolling with grid_'s content, but paint order is
    // separate from that and still follows child-insertion order, so
    // grid_'s content would otherwise be drawn over it once scrolled.
    // See StatusBar::Root()'s own comment.
    lv_obj_move_foreground(status_bar_.Root());
}

DashboardScreen::~DashboardScreen() { lv_obj_del(root_); }

}  // namespace homedeck
