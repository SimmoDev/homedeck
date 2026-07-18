#pragma once

namespace homedeck {

// Reads and logs why the *previous* boot ended, and reports on any core
// dump captured during a preceding panic - see
// docs/decisions/ADR-0013-crash-and-reboot-diagnostics.md. Persists both
// via Storage so a later Web UI (not yet built) can present "last reboot
// reason" without needing to catch the exact boot that produced it.
//
// Must be called after nvs_flash_init().
void LogCrashDiagnostics();

}  // namespace homedeck
