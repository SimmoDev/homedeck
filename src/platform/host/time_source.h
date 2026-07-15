#pragma once

#include "platform/time_source.h"

namespace homedeck {

class HostTimeSource : public TimeSource {
public:
    std::chrono::system_clock::time_point Now() const override {
        return std::chrono::system_clock::now();
    }
};

}  // namespace homedeck
