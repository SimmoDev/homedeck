# ADR-0017: Partition Table

## Status

Accepted

## Context

M1 unblocked flash exhaustion with ESP-IDF's built-in single-app-large
table (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`, 1500K) as an explicitly
pragmatic, temporary measure — not the real OTA A/B scheme, which
[docs/roadmap.md](../roadmap.md)'s OTA item and
[ADR-0013](ADR-0013-crash-and-reboot-diagnostics.md) already flagged as
explicit M2 scope. With M2's Wi-Fi/ESP-Hosted stack now linked in
(`esp_wifi_remote`, `esp_hosted`, `esp_http_server`, `wpa_supplicant`,
`mbedtls`), that table dropped to 1% free, confirmed via a real build —
see [hardware.md](../architecture/hardware.md#wi-fi-bring-up).

Every M2 roadmap item still ahead needs partition-table space the current
table doesn't reserve: OTA's real A/B scheme itself; a dedicated core
dump partition, which ADR-0013 already says must be "planned alongside
the OTA A/B scheme... not added later"; an `nvs_keys` partition for NVS
encryption ([ADR-0010](ADR-0010-secret-storage.md)); and a FAT +
`wear_levelling` partition for cached data, rotating logs, and the Web
Management UI's compiled Svelte bundle
([ADR-0012](ADR-0012-storage-tiers.md),
[ADR-0002](ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach) —
one partition serves all three, not separate technology choices).
Repartitioning once now, with real headroom reserved for each, avoids
revisiting the layout piecemeal as each of those features actually gets
built.

This ADR covers the partition table only. It does not implement OTA
update logic, core dump capture, NVS encryption, or the FAT filesystem/
Storage service — those remain separate, not-yet-started M2 roadmap
items building against the space reserved here.

## Decision

**Custom partition table** (`firmware/partitions.csv`), replacing the
built-in single-app table, on the confirmed 16MB flash:

| Partition  | Type/subtype    | Size      | Purpose |
|------------|------------------|-----------|---------|
| `nvs`      | data/nvs         | 76KB      | Settings and secrets — *not* Wi-Fi credentials, which live on the C6 co-processor's own flash under this project's `esp_wifi_remote` stack (see [hardware.md#wi-fi-bring-up](../architecture/hardware.md#wi-fi-bring-up)). The boot log's own `WiFi data` label for this partition is misleading but permanent and expected: confirmed in ESP-IDF's bootloader source (`bootloader_utility.c`) that this string is hardcoded for *any* `data`/`nvs`-subtype partition purely by subtype, regardless of name or actual contents — not specific to this project, and not fixable by renaming the partition. Sized larger than the previous table's 24KB not for its own sake, but because that's what lands the table on the 64KB boundary `ota_0` (below) requires, with zero padding wasted getting there — see [Partition ordering and alignment](#decision-partition-ordering-and-alignment) below |
| `nvs_keys` | data/nvs_keys    | 4KB       | NVS encryption keys (ADR-0010) — reserved, not yet activated |
| `otadata`  | data/ota         | 8KB       | OTA slot selection (standard fixed size) |
| `phy_init` | data/phy         | 4KB       | RF calibration data (unchanged) |
| `ota_0`    | app/ota_0        | 4MB       | OTA slot A |
| `ota_1`    | app/ota_1        | 4MB       | OTA slot B |
| `coredump` | data/coredump    | 256KB     | Panic core dumps (ADR-0013) — reserved, not yet activated |
| `storage`  | data/fat         | 7.625MB   | Cached data, rotating logs, Web UI static bundle (ADR-0012, ADR-0002) — reserved, not yet mounted |

**`ota_0`/`ota_1` sized at 4MB each — options considered:**
- 2MB (~1.35x current ~1.48MB usage) — tighter; the largest planned M3-M6
  module (Harmony) alone could plausibly force a second repartition
  before M6.
- 3MB (~2x current usage) — comfortable middle ground.
- 4MB (~2.7x current usage) — decided. 16MB total flash makes the extra
  headroom cheap, and the goal of this ADR is specifically to avoid a
  second repartition once real modules start landing in M3-M6.

**Pure A/B (`ota_0`/`ota_1`), no `factory` partition — decided.** A
`factory` slot (a third, never-OTA-updated app partition for field
recovery) is the standard ESP-IDF default when OTA is enabled via the
built-in "two OTA" table, but it costs a full extra app-slot of flash and
the image goes stale over time unless deliberately re-flashed. Pure A/B
with bootloader app-rollback (rolling back to the previous OTA slot if a
new image fails to boot) is the standard modern pattern for products that
implement real rollback support — rejected here only as *not yet
implemented*, not as a reason to keep a `factory` slot; this table
doesn't block adding rollback support later.

**`storage` sized to the remainder (7.625MB) rather than a fixed smaller
number** — its three consumers (cache, logs, Web UI bundle) don't have
measured sizes yet, and ADR-0012 already defers exact cache-retention
rules to implementation time. There's no other use for the remaining
budget on a 16MB chip; if `ota_0`/`ota_1` ever need to grow past 4MB,
that's a future repartition against this same file, not blocked by
allocating the remainder now.

## Decision: partition ordering and alignment

ESP-IDF requires `app`-type partitions (`ota_0`/`ota_1`) to start on a
64KB-aligned flash offset; `data`-type partitions only need 4KB sector
alignment. `gen_esp32part.py` silently pads up to the next 64KB boundary
before an `app` partition if the preceding `data` partitions don't land
on one exactly — real, build-verified behavior: sizing `nvs` at 24KB
(matching the previous single-app table) leaves the `data` cluster ending
at an offset that isn't 64KB-aligned, and the resulting padding pushes
the table past the 16MB flash budget entirely ("partitions table occupies
16.1MB... does not fit").

**Options considered:**
- Accept the padding as wasted space, and shrink `storage` to compensate
  — works, but "waste tens of KB for no reason" is worse than the
  alternative below for zero cost.
- Reorder so a `data`-type partition that doesn't otherwise need a fixed
  size (here, `nvs`) absorbs exactly the gap, sized so the cluster of
  `data` partitions before `ota_0` lands precisely on a 64KB boundary —
  decided. `nvs` grows from the previously-planned 24KB to 76KB, which is
  more headroom than currently needed but not wasted: it's real,
  usable NVS space, not padding.

This is why `nvs`'s size in the table above isn't a round decimal number
— it's derived from the alignment requirement, not picked independently.
Any future edit to this table that changes a `data` partition's size
ahead of `ota_0` must re-verify this still lands on a 64KB boundary via a
real build, not by hand-computed offsets — this exact mistake is easy to
reintroduce silently otherwise.

## Consequences

- [hardware.md](../architecture/hardware.md#on-device-dashboard) and
  [docs/roadmap.md](../roadmap.md) no longer describe the OTA A/B scheme
  as pending — the table exists; the OTA client, core dump capture, NVS
  encryption, and FAT filesystem mount are still separate implementation
  work against the space reserved here.
- Flashing this table changes partition offsets (new partitions
  interspersed before `nvs`), so existing on-device NVS content doesn't
  carry over from the previous single-app table — a one-time,
  expected consequence of this change, not a defect. Wi-Fi credentials
  are unaffected either way, since they're confirmed to live on the C6
  co-processor's own flash, not this table (see
  [hardware.md#wi-fi-bring-up](../architecture/hardware.md#wi-fi-bring-up)).
- `ota_0`/`ota_1` sizing is a judgment call against currently-unbuilt
  modules, not a measurement — worth revisiting if a future module (or
  the Web UI's own compiled backend, if that ever needs to move off
  `storage`) turns out to need meaningfully more than 4MB.
