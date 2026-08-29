#pragma once

#include "core/logger.h"
#include "debug_kodi_backend.h"
#include "platform/host/audio_output.h"
#include "platform/host/battery_reader.h"
#include "platform/host/network_status.h"
#include "platform/user_activity_source.h"
#include "ui/navigation.h"

#include "lvgl.h"

namespace homedeck::sim {

// Wraps the real LvglUserActivitySource with a settable override - it has
// no separate fake backend to begin with (unlike HostBatteryReader/
// HostNetworkStatus, real UserActivitySource is identical on both targets,
// see ui/lvgl_user_activity_source.h), and LVGL has no "pretend N minutes
// passed" API, only lv_display_trigger_activity() (resets to just-active).
// Debug scaffolding, not a second real platform/host/ backend, same
// reasoning ForceOtaFailure (main.cpp's force_ota_failure bool) already
// follows.
class DebugOverridableUserActivitySource : public UserActivitySource {
public:
    enum class ForcedLevel { kNone, kIdle, kSleeping };

    explicit DebugOverridableUserActivitySource(UserActivitySource& real) : real_(real) {}

    uint32_t MillisecondsSinceLastActivity() const override {
        switch (forced_) {
            case ForcedLevel::kIdle:
                return kForcedIdleOnlyMs;
            case ForcedLevel::kSleeping:
                return kForcedSleepingMs;
            case ForcedLevel::kNone:
            default:
                return real_.MillisecondsSinceLastActivity();
        }
    }

    void SetForced(ForcedLevel level) { forced_ = level; }

private:
    // Past kIdleTimeoutMs but short of kSleepTimeoutMs (see
    // core/power_manager.cpp) so this level parks at Idle rather than
    // cascading into Sleeping on a later tick - a known, accepted coupling
    // to those placeholder values, same convention kForcedSleepingMs below
    // already follows.
    static constexpr uint32_t kForcedIdleOnlyMs = 60000;
    // Past any real placeholder timeout, including Sleep's - see
    // core/power_manager.cpp.
    static constexpr uint32_t kForcedSleepingMs = 24u * 60 * 60 * 1000;

    UserActivitySource& real_;
    ForcedLevel forced_ = ForcedLevel::kNone;
};

// A bottom-anchored flex column all the CreateTestXButton() functions
// below attach to, so a new debug button just gets added to the flow - no
// manually-chosen pixel offset to pick, and no need to renumber other
// buttons' offsets if one is removed. COLUMN_REVERSE so each new call
// appends above the previous one, closest-to-edge first. Cross axis
// (horizontal, for a column flow) must be CENTER, not END - END
// right-aligns each button against the panel's own content-sized width
// instead of centering it.
lv_obj_t* CreateTestButtonPanel(lv_obj_t* parent);

// WifiSetupScreen deliberately omits the home affordance (see ui.md's
// Navigation model) - correct on real hardware, since an unprovisioned
// device shouldn't let the user bail back to a dashboard with no network,
// but it leaves the simulator with no way back to the dashboard once
// CreateTestWifiSetupNavButton below navigates here. Parent this to
// wifi_setup_screen.Root() itself, not the test button panel - this
// screen's controls are the only ones visible once it's loaded. Removed
// once a trigger exists, same as CreateTestWifiSetupNavButton below.
void CreateTestBackToDashboardButton(lv_obj_t* parent, Navigation& navigation);

// Temporary test-only wiring to reach the Wi-Fi setup screen - the
// simulator has no real SoftAP "not provisioned" path to trigger it
// naturally (see ui/screens/wifi_setup_screen.h). Removed once a
// trigger exists.
void CreateTestWifiSetupNavButton(lv_obj_t* parent, Navigation& navigation);

// Temporary test-only wiring to exercise StatusBar's and
// NetworkStatusWidget's disconnected rendering - HostNetworkStatus
// defaults to connected with nothing to disconnect it. Removed once a
// trigger exists.
void CreateTestWifiDisconnectButton(lv_obj_t* parent, HostNetworkStatus& network_status);

// Not temporary - there's no other way to manually exercise AudioOutput
// in the simulator, a lasting dev hook for that hardware signal the same
// way the battery/network buttons below are for theirs.
void CreateTestPlayToneButton(lv_obj_t* parent, HostAudioOutput& audio_output);

// Not temporary - toggles DebugKodiBackend between transparent and
// answering as a fake Kodi, so NowPlayingScreen / KodiRemoteScreen's
// connected-only content (hidden until KodiClient reaches kConnected)
// can be seen in the simulator without a Kodi on the LAN. See
// debug_kodi_backend.h for the fake's scope.
void CreateTestFakeKodiButton(lv_obj_t* parent, DebugKodiBackend& kodi_backend);

// Temporary test-only wiring proving LowBatteryMonitor/NotificationBanner
// end to end - HostBatteryReader is a fixed-then-adjustable mock (see
// platform/host/battery_reader.h) that never naturally crosses the
// low-battery threshold on its own, so there's no other way to see the
// notification flow run in the simulator. Removed once a widget
// (weather) or some other trigger exists to exercise this naturally.
void CreateTestLowBatteryButton(lv_obj_t* parent, HostBatteryReader& battery_reader);

// Same reasoning as CreateTestLowBatteryButton above, for
// CriticalBatteryMonitor/PowerManager's kError transition instead - 2% is
// below CriticalBatteryMonitor::kCriticalThresholdPercent (5), the same
// margin style that button's 10 uses against LowBatteryMonitor's 15.
void CreateTestCriticalBatteryButton(lv_obj_t* parent, HostBatteryReader& battery_reader);

// Temporary test-only wiring proving GET /api/diagnostics' external-power
// field end to end - HostBatteryReader's external-power flag never
// changes on its own. Removed once a Power Management screen exists
// to exercise this.
void CreateTestExternalPowerButton(lv_obj_t* parent, HostBatteryReader& battery_reader);

// Temporary test-only wiring proving LowBatteryMonitor doesn't fire (and
// GET /api/diagnostics' batteryPresent field reflects reality) when no
// battery is installed. Removed once a Power Management screen
// exists to exercise this.
void CreateTestBatteryPresentButton(lv_obj_t* parent, HostBatteryReader& battery_reader);

// Temporary test-only wiring proving the Web UI's OTA page surfaces a
// real upload failure, not just the success path - see
// docs/architecture/simulator.md's OTA mock description. Removed once a
// Power Management screen (or similar) exists to exercise this.
void CreateTestForceOtaFailureButton(lv_obj_t* parent, bool& force_failure);

void CreateTestTriggerIdleButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source);
void CreateTestTriggerSleepingButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source);
void CreateTestTriggerActiveButton(lv_obj_t* parent, DebugOverridableUserActivitySource& source);

// Temporary test-only wiring proving the Web UI's Logs section renders
// real entries without needing multiple simulator restarts to accumulate
// them - see docs/decisions/ADR-0019-structured-logging.md. Removed once
// a naturally-occurring event exists to exercise this in the
// simulator (there's no Wi-Fi/mDNS bring-up here to log, unlike
// firmware).
void CreateTestLogEntryButton(lv_obj_t* parent, Logger& logger);

}  // namespace homedeck::sim
