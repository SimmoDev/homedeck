#include "ui/screens/dashboard_screen.h"

namespace homedeck {

DashboardScreen::DashboardScreen(EventBus& event_bus, BatteryReader& battery_reader, NetworkStatus& network_status)
    : root_(lv_obj_create(nullptr)),
      status_bar_(root_, event_bus, battery_reader, network_status),
      grid_(root_) {}

}  // namespace homedeck
