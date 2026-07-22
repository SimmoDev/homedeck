#pragma once

#include "core/storage.h"

namespace homedeck {

// Reads and logs why the *previous* boot ended, and reports on any core
// dump captured during a preceding panic - see
// docs/decisions/ADR-0013-crash-and-reboot-diagnostics.md. Persists both
// via Storage so the Web UI's diagnostics page (`core/diagnostics_routes.h`)
// can present "last reboot reason" without needing to catch the exact
// boot that produced it.
//
// Takes the caller's own Storage rather than constructing its own -
// FirmwareCacheStore mounts the `storage` FAT partition, and a second,
// independent FirmwareCacheStore mounting that same partition while the
// caller's own is already active is a real conflict, not just redundant
// construction.
void LogCrashDiagnostics(Storage& storage);

}  // namespace homedeck
