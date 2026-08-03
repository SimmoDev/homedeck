#pragma once

#include "core/admin_auth_service.h"
#include "platform/http_server.h"

#include <functional>
#include <optional>
#include <string>

namespace homedeck {

// Clears stored Wi-Fi credentials and reboots, so the device re-enters
// SoftAP setup. Injected the same way OtaWriter/DeviceNameValidateFn are
// (core/ota_routes.h, core/settings_routes.h), but must not block and
// must not tear down Wi-Fi synchronously - firmware schedules the real
// esp_wifi_restore() (see docs/architecture/hardware.md#wi-fi-bring-up
// for why that's the correct call, not erasing the P4's own nvs region)
// together with esp_restart(), deferred past the handler returning, the
// same reason OtaRebootFn's own reboot is deferred (see homedeck.cpp's
// ScheduleReboot()) but sharper here: esp_wifi_restore() itself severs
// the STA association carrying the very HTTP response back to the
// caller, not just the later reboot - calling it synchronously leaves the
// Web UI's request hanging forever with no response ever arriving. A reboot
// afterward isn't optional the way OTA's is either - restoring Wi-Fi
// settings only clears stored credentials; the device only re-enters
// SoftAP setup inside InitWifiAndCheckStoredCredentials(), which runs
// once at boot, so without rebooting the normal (non-setup) reconnect
// path in wifi_setup.cpp would just retry against the now-empty config
// indefinitely.
//
// Returns the SoftAP SSID the device will broadcast once it reboots
// (e.g. "HomeDeck-A1B2C3", the same value WifiCredentialsCheck::ap_ssid
// already carries from InitWifiAndCheckStoredCredentials() - computed
// from the device's own MAC, so it's known before the reboot even
// happens) on success, so the Web UI can tell the user exactly which
// network to reconnect to instead of a generic "look for SoftAP"
// message - the browser's own session is about to become unreachable at
// its current address, so this is the last chance for the device to say
// where to find it next. std::nullopt only if scheduling itself fails.
// The simulator has no Wi-Fi credential storage of its own to clear
// (wifi_setup.cpp is 100% firmware-only, see tests/README.md) and
// nothing to reboot, so it's a no-op returning its own fixed placeholder
// SSID there.
using WifiResetFn = std::function<std::optional<std::string>()>;

// Registers POST /api/wifi/reset - admin-only via auth.RequireAuth().
// Added as a diagnostic aid for reproducing the intermittent Wi-Fi-
// connect crash (see hardware.md's "Known, not yet root-caused" note)
// without a full tools/factory-reset.sh erase-and-reflash cycle each
// time. Deliberately narrow: clears only the Wi-Fi credentials living on
// the C6 co-processor's own flash - Core's own Storage (settings,
// secrets, the admin password) is untouched. Not the same as the M7
// "Web Management UI factory-reset option" roadmap item, whose own scope
// (a dedicated action vs. part of a broader reset) is still undecided -
// see roadmap.md.
//
// Reboots automatically once reset() schedules it - unlike OTA's own
// explicit, separately-confirmed "Reboot now" step, a second confirmed
// action isn't meaningfully clickable here: the connection carrying this
// endpoint's own response is already gone by the time a user could react
// to it, and (see WifiResetFn's own comment above) the reboot is required
// for this action to do anything at all, not an optional follow-up. On
// success, responds with {"status":"ok","apSsid":"..."}.
void RegisterWifiRoutes(HttpServer& server, AdminAuthService& auth, WifiResetFn reset);

}  // namespace homedeck
