#pragma once

#include "platform/battery_reader.h"

namespace homedeck {

// A mock value, adjustable via SetPercent() - defaults to 76, a normal
// mid-charge number a dashboard widget doesn't need to move.
// LowBatteryMonitor (see core/low_battery_monitor.h) needs the value to
// actually drop below its threshold to be testable in the simulator, so
// it has to be settable, not fixed. docs/architecture/simulator.md's own
// note about a future debug control for battery isn't built here - this
// is just a plain setter a temporary test trigger calls (see
// simulator/main.cpp).
class HostBatteryReader : public BatteryReader {
public:
    int ReadPercent() const override { return percent_; }
    void SetPercent(int percent) { percent_ = percent; }

    bool IsExternalPowerConnected() const override { return external_power_connected_; }
    void SetExternalPowerConnected(bool connected) { external_power_connected_ = connected; }

    bool IsBatteryPresent() const override { return battery_present_; }
    void SetBatteryPresent(bool present) { battery_present_ = present; }

private:
    int percent_ = 76;
    bool external_power_connected_ = false;
    bool battery_present_ = true;
};

}  // namespace homedeck
