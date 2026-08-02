# ADR-0023: Settings/Config/Backup REST API

## Status

Accepted — the durable fix this ADR's Decision section named and
deferred (giving `SecretStore` its own NVS partition) is delivered by
[ADR-0027](ADR-0027-secret-store-partition-separation.md); the reserved-
key guard described below stays in place as a second-layer safeguard,
not because it's still the only protection.

## Context

[web-ui.md](../architecture/web-ui.md) names Settings, Module
configuration, and Backups as in-scope Web UI responsibilities, but none
of it was built yet - only auth, diagnostics, and OTA existed.
`Storage::SetSetting/GetSetting/EraseSetting` (`core/storage.h`) already
existed and was proven on real hardware (`crash_diagnostics.cpp` uses it
for `reset_reason`/`has_core_dump`); this closes the gap by exposing it
over HTTP, adding the one capability it was missing (enumerating
everything, for a backup export), and giving it a first real,
user-facing consumer - a device name setting, replacing the previously
hardcoded `"homedeck"` mDNS hostname.

**Explicitly out of scope for this pass**, each either blocked on other
work or big enough to be its own follow-up: Wi-Fi management
(view/change post-provisioning - firmware-only today, needs its own
simulator-parity design), WebSockets/live updates (blocked on an
unrelated civetweb cross-task-dispatch-safety question
[web-ui.md](../architecture/web-ui.md) already flags as unresolved),
factory-reset (roadmap itself says "not yet designed, scope isn't
decided").

## Security finding that shaped this design

`SettingsStore` and `SecretStore` are separate C++ types, but on
firmware `FirmwareSettingsStore` and `FirmwareSecretStore` write into
the exact same physical NVS partition and the same namespace-per-module
key space - both call plain `nvs_open(ns, ...)` against the default
partition, and `FirmwareSecretStore`'s own header already admitted it:
*"a caller must not reuse the same (ns, key) pair through both stores
for the same module, since nothing here prevents that collision at the
NVS layer."* `Storage::SetSetting` and `SetSecret` both wrap values in
the identical schema-versioned envelope first, so there's no way to
distinguish a secret from a setting by inspecting stored bytes.

This mattered concretely because the admin password hash is stored at
exactly `SecretStore("core", "admin_pw_hash")`. A fully generic,
unguarded `POST /api/settings` (or a listing-backed `GET
/api/settings`/`GET /api/backup`) would have let an authenticated caller
read or silently overwrite the admin password hash through the "wrong
door" - most realistically via `POST /api/backup/restore` replaying an
old or externally-sourced backup file, not through anything that looks
like an attack.

## Decision

**A reserved-key guard**, not a partition split, for this pass.
`AdminAuthService::kModuleId`/`kPasswordKey` were made public so
`Storage` could reference the exact reserved pair without duplicating
the literal strings. `Storage::SetSetting` rejects (returns `false`)
writes to that pair, and `Storage::ListAllSettings()` (the new
enumeration method, backing both `GET /api/settings` and `GET
/api/backup`) silently excludes it - checked at the `Storage` layer
itself, not only in the route handler, so every current and future call
path is covered. This is a single, enumerable, explicitly-checked
constant today, not the "convention everyone must remember" pattern
this project's ADRs otherwise reject. `POST /api/settings`'s own handler
(`settings_routes.cpp`) also checks it directly ahead of calling
`SetSetting`, so this specific, deliberate rejection responds `403
reserved_key` - distinguishable from the generic `500 write_failed` a
real storage fault also produces - rather than relying solely on
`SetSetting`'s own `false`
return falling through to that same generic path.

**The durable fix - giving `SecretStore` its own NVS partition on
firmware, mirroring what `HostSecretStore` already does with a separate
directory - is deliberately deferred, not silently dropped.** It's real
work: a partition-table change plus a `FirmwareSecretStore` rewrite to
`nvs_open_from_partition()`, with a one-time cost (re-flashing wipes
existing NVS, the same accepted cost [ADR-0017](ADR-0017-partition-table.md)
already documents from the last repartition). This naturally belongs
with [ADR-0018](ADR-0018-staged-security-hardening.md)'s staged security
hardening or M3's first real module credential, whichever comes first.

**`SettingsStore` gains `ListAll()`** (`std::vector<SettingsEntry>`,
namespace+key+raw value) - `HostSettingsStore` via a two-level
`std::filesystem::directory_iterator` (each key is already one file on
disk), `FirmwareSettingsStore` via ESP-IDF's already-linked
`nvs_entry_find`/`nvs_entry_next`/`nvs_entry_info` (namespace_name=NULL
enumerates every namespace in one pass - no new component dependency).
`SecretStore` gets no equivalent method - that, plus the reserved-key
guard above, is what keeps secrets out of the generic listing/backup
path.

**Five endpoints** (`src/core/settings_routes.h`/`.cpp`), all admin-only
via `AdminAuthService::RequireAuth()`. `HttpServer` only supports
`kGet`/`kPost` and exact literal paths (no path params, no existing
query-string convention), so mutations that would naturally be
PUT/DELETE are POST actions instead, matching `POST /api/ota/reboot`'s
existing precedent:

- `GET /api/settings` - every entry as a JSON array
- `POST /api/settings` - body `{module,key,value,schemaVersion}`;
  rejects module/key over 15 characters (`NVS_KEY_NAME_MAX_SIZE - 1`)
  explicitly, so this new public endpoint behaves identically on both
  targets instead of silently succeeding on the simulator (no such cap
  there) and failing on firmware
- `POST /api/settings/erase` - body `{module,key}`
- `GET /api/backup` - the same data as `/api/settings`, wrapped as
  `{"settings":[...]}`, downloaded as `homedeck-backup.json`
- `POST /api/backup/restore` - the same shape re-uploaded, replayed
  through the same guarded `SetSetting` path entry-by-entry (so restore
  automatically inherits the reserved-key protection); not atomic - no
  transaction concept exists anywhere else in this codebase either -
  reports `{"applied":N,"failed":[...]}` so a partial restore is visible
  rather than silently incomplete

**Device name is the first real setting**, replacing the hardcoded
`"homedeck"` mDNS hostname. `RegisterSettingsRoutes` takes an optional
injected `DeviceNameChangedFn` callback, invoked only for
`(module="core", key="device_name")` writes, before persisting. On
firmware it validates the value against RFC 1035/6763 label rules
(charset, ≤63 chars) and re-calls `mdns_hostname_set()` immediately -
live-apply, no reboot required, since ESP-IDF's mdns component supports
re-announcing. Returning `false` makes the route respond 400 rather than
persisting a value that was rejected. On the simulator the callback is
omitted entirely (no mDNS to update) - keeps `settings_routes.cpp` fully
portable, matching `ota_routes.cpp`'s `OtaWriter` injection pattern for
the same reason. `POST /api/backup/restore`'s generic replay loop does
**not** invoke this callback - an accepted simplification, not an
oversight: restoring a backup containing a device name change persists
correctly but only takes effect on the next reboot, since making restore
aware of per-key semantics would tangle a deliberately generic replay
loop for a rare, self-inflicted-only case.

## Consequences

- This project now carries an interim, explicitly-scoped security
  mitigation (the reserved-key guard) rather than the structurally
  correct fix. Extending it whenever a new `SecretStore` key is
  introduced is a real, manual step - flagged here so it isn't
  forgotten, not left to be rediscovered.
- `docs/roadmap.md`'s Status bar item already separately tracks
  `StatusBar`'s clock label not reading `TimeSource` immediately: this
  ADR's `device_name` work doesn't touch that.
- **Confirmed end to end against the simulator** (`curl` against a real
  running `HostHttpServer`): setup → set device name → list → backup
  download → attempted `admin_pw_hash` overwrite via both `POST
  /api/settings` and `POST /api/backup/restore` (both rejected, original
  password hash confirmed unchanged and still able to log in) → erase.
- **Confirmed on real hardware** (Tab5 K145 reference unit, over the
  LAN) - the same sequence, plus the live mDNS re-announce (serial log:
  `mDNS re-announced as <name>.local`, no reboot); the reserved-key
  rejection is reliable across repeated attempts, not just occasionally
  correct.
