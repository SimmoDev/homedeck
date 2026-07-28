#pragma once

#include "platform/user_activity_source.h"

namespace homedeck {

// The one real UserActivitySource implementation - lives here, not in
// src/platform/, because it calls lv_display_get_inactive_time(), which
// Core must never depend on directly (same reasoning as
// ui/navigation.h's own comment on why Navigation lives in ui/ despite
// being a Core responsibility conceptually). Identical on firmware and
// simulator - both run real LVGL, just different display drivers - so
// unlike most platform/ interfaces, there's no host/firmware split.
class LvglUserActivitySource : public UserActivitySource {
public:
    uint32_t MillisecondsSinceLastActivity() const override;
};

}  // namespace homedeck
