# ADR-0012: Storage Tiers

## Status

Accepted

## Context

[core.md](../architecture/core.md)'s Storage responsibility says Core
persists "configuration, cached data, and credentials" without specifying
which physical mechanism backs which category. This matters for real
reasons, not just tidiness: NVS (already committed to for credentials in
[ADR-0010](ADR-0010-secret-storage.md)) is designed for small key-value
data, not general blob storage — mixing in larger cached datasets (e.g. a
Home Assistant entity list, which can run to tens of KB) would be a real
anti-pattern, not just inefficient. Separately, CLAUDE.md's target hardware
list explicitly names microSD as a capability the firmware should use, but
[hardware.md](../architecture/hardware.md) had miscategorized it as out of
scope alongside genuinely-unused expansion hardware — corrected alongside
this ADR.

## Decision

**Options considered for the general split:**
- Everything in NVS — rejected outright; NVS isn't designed for the size or
  access pattern of cached device lists or dashboard data, and forcing it
  would be a real anti-pattern, not a simplification.
- A single internal flash filesystem for everything (no SD tier) — simpler
  (one mechanism to implement), but ignores microSD despite CLAUDE.md
  explicitly wanting it used, and would put cached data and backups in
  competition with firmware and dual OTA partitions for the same 16MB
  internal flash budget.
- A three-tier split by data characteristics (decided).

**Decided:** three tiers, chosen by data characteristics rather than
convenience:

- **NVS:** small, sensitive, frequently-read data — the admin password
  hash, small settings/preferences, and (from M3 onward) module
  credentials. What NVS is actually designed for. Encryption for this
  tier is staged by [ADR-0018](ADR-0018-staged-security-hardening.md),
  not activated from the start — see that ADR for why. Wi-Fi credentials
  are not part of this tier: they live on the C6 co-processor's own flash
  (see [hardware.md](../architecture/hardware.md#wi-fi-bring-up)).
- **Internal flash filesystem** (see [Decision: filesystem
  choice](#decision-internal-flash-filesystem-choice) below for which one):
  structured data that needs to survive reboots but isn't large or
  inherently removable — cached dashboard/device-list data (the "cached
  configuration," "cached device lists," "cached dashboard data" CLAUDE.md's
  Offline Behaviour requirements ask for), and bounded/rotating logs. This
  is why the cache survives a reboot while offline rather than only
  surviving while the device happens to stay powered — a plain in-RAM
  cache would lose everything on a reboot that happens while offline,
  which would violate the actual intent of "cached data" surviving
  unavailability.
- **microSD (optional):** larger, user-facing, removable data. **Not
  backups** — see [Backup delivery](#decision-backup-delivery) below for
  why that's a Web UI download instead. This tier's actual use: **extended
  log archival** — the internal flash filesystem's logs are bounded/rotating
  (see above), so older diagnostic history that would otherwise be evicted
  can optionally be archived to microSD if present, genuinely benefiting
  from being larger-capacity and removable the way the internal tier isn't.
  Per CLAUDE.md's "work fully using stock Tab5 hardware" requirement, and
  since a card is not guaranteed present even when the slot is, nothing on
  this tier is required for core functionality — SD-backed features
  degrade gracefully (clearly indicating "no card present," not silently
  failing) when no card is inserted.

**Cache retention:** cached data (device lists, dashboard data) needs a
bounded retention policy rather than growing unbounded — exact eviction
rules (age-based, size-based, or both) are deferred to M2 implementation,
consistent with how other numeric thresholds in this project are handled
(see [ADR-0005](ADR-0005-power-and-sleep-model.md)), but the *requirement*
that it be bounded is decided now, not left implicit.

## Decision: Internal flash filesystem choice

LittleFS is **not** part of ESP-IDF — unlike SPIFFS and FAT, which ship
in-tree, the common ESP-IDF LittleFS integration (`joltwallet/esp_littlefs`)
is a third-party component pulled from the ESP Component Registry, the same
category of dependency-addition decision that gets scrutiny for
nlohmann::json and civetweb elsewhere in
[ADR-0002](ADR-0002-technology-stack.md).

**Options:**
- SPIFFS — in-tree, zero new dependency, but no real directory support
  (flat namespace), known to scale poorly with file count, and generally
  considered legacy within the ESP-IDF ecosystem for new projects.
- FAT + `wear_levelling` — in-tree, zero new dependency, real directory
  support, a wear-leveling component purpose-built for flash write
  endurance, and the most heavily-used path in the ESP-IDF ecosystem for
  exactly this "structured files on internal flash" need.
- LittleFS (third-party, e.g. `joltwallet/esp_littlefs`) — real
  wear-leveling and power-loss resilience, POSIX-like API, well-regarded,
  but an external dependency not shipped with ESP-IDF.

**Decided: FAT + `wear_levelling`.** It's in-tree — consistent with the
"minimise external dependencies" discipline already applied to every other
technology choice in [ADR-0002](ADR-0002-technology-stack.md) — and its
purpose-built wear-leveling component directly matters here, since cached
data plus rotating logs means non-trivial write frequency over the
device's lifetime; flash write endurance is a real concern, not a
hypothetical one. LittleFS is rejected as an unnecessary third-party
dependency when an in-tree option covers the same need adequately. SPIFFS
was considered and rejected as the weaker in-tree option given its lack of
real directory support and legacy status.

## Decision: Backup delivery

**Context:** "Backups" has been an unelaborated item in
[web-ui.md](../architecture/web-ui.md)'s scope since M0. With microSD now
resolved as available for larger removable data, this can finally be
answered concretely.

**Options:**
- Backup writes to microSD if present, falling back to a Web UI download
  if no card is inserted — two code paths, but a more native experience
  when a card is available (no browser round-trip for a potentially larger
  backup file).
- Always a Web UI download (a JSON export of Core config + module
  settings), regardless of whether an SD card is present — one code path,
  simpler, consistent behavior regardless of hardware configuration.

**Decided:** always a Web UI download. A single code path is worth more
than the marginal convenience of writing directly to SD, especially since
backup size (config + settings, not cached data) is small enough that a
browser download has no real disadvantage — the SD-native path would add
real implementation complexity (detecting card presence, handling
write failures, a different UI flow) for a case that doesn't clearly need
it. Restore is the same JSON file re-uploaded through the Web UI. This can
be revisited if backup scope ever grows to include something genuinely
too large for a convenient download (not currently the case).

**Secrets are excluded from this export.** The admin password hash and
any future module credential are not "config" in the sense this export
covers — a backup file is handled far more casually than the device
itself (emailed, dropped in cloud storage), so it must not become a
lower-friction way to exfiltrate what NVS encryption is eventually meant
to protect. This is enforced structurally by routing secrets through
`SecretStore`, not `SettingsStore` — see
[ADR-0010](ADR-0010-secret-storage.md#decision-secret-storage-interface)
— rather than left to the export code's discretion.

## Decision: Storage namespacing

**Context:** none of the tiers above specify whether one module's stored
data is isolated from another's. Without an explicit namespacing
convention, two modules could silently collide on the same key — a
hidden coupling channel that violates
[ADR-0003](ADR-0003-module-architecture.md)'s "modules never communicate
with each other" principle in spirit, even though no direct function call
is involved.

**Decided:** every module-owned key (settings, cached data) is namespaced
by module ID, enforced by Core's storage service itself — a module cannot
read or write another module's namespace, not merely asked not to by
convention. This is the same reasoning already applied elsewhere in this
project: a correctness requirement that depends on every future module
author remembering a convention correctly is exactly the kind of thing
that should be structurally enforced instead (see the same logic in
[ADR-0011](ADR-0011-lvgl-thread-safety.md)'s rejection of leaving LVGL
thread-safety to each screen controller's discipline).

## Consequences

- [core.md](../architecture/core.md) and
  [hardware.md](../architecture/hardware.md#microsd) state the resulting
  tiering without repeating this reasoning.
- Core's storage service interface must take a module identity as part of
  every call (or be handed a pre-scoped accessor per module at
  registration), not a global flat key space — a concrete interface
  detail for M2, not fixed further by this ADR.
- [web-ui.md](../architecture/web-ui.md)'s "Backups" scope item is no
  longer vague — config + module settings, downloadable JSON, no SD or
  cloud involvement.
- Any future feature that wants to persist data must be placed into one of
  these three tiers by its actual characteristics (size, sensitivity,
  removability need), not by whichever mechanism is most convenient to
  reach for at the time.
