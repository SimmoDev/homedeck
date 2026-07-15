#pragma once

#include "platform/battery_reader.h"

namespace homedeck {

// A fixed mock value. docs/architecture/simulator.md documents battery
// as something that "can be manipulated from a debug control, rather
// than always reporting a fixed value" - that control isn't built here.
// There's no real consumer for *adjustable* mock battery yet (that's
// power-management testing - low-battery warnings, charging faults -
// which is M2 scope); a dashboard widget rendering a number doesn't need
// it to move.
class HostBatteryReader : public BatteryReader {
public:
    int ReadPercent() const override { return 76; }
};

}  // namespace homedeck
