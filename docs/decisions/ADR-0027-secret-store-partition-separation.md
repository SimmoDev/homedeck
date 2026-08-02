# ADR-0027: SecretStore Partition Separation

## Status

Accepted. Delivers the durable fix
[ADR-0023](ADR-0023-settings-backup-api.md#decision) deliberately
deferred, and extends the partition table
[ADR-0017](ADR-0017-partition-table.md) established.

## Context

`FirmwareSecretStore` and `FirmwareSettingsStore` shared the same
physical NVS partition and namespace-per-module_id scheme on firmware -
`FirmwareSettingsStore::ListAll()` (`nvs_entry_find(NVS_DEFAULT_PART_NAME,
nullptr, NVS_TYPE_BLOB, &it)`) enumerated every blob in that partition
regardless of which store had written it. The only thing keeping a
secret out of `GET /api/settings`/`GET /api/backup` was `Storage`'s own
reserved-key guard - a single hardcoded `(module_id, key)` pair for the
admin password hash, requiring a developer to remember to add a new
entry for every future secret. M3 (Harmony) introduces the project's
first real module credential (hub IP/password) through `SecretStore`,
which is exactly the trigger [ADR-0018](ADR-0018-staged-security-hardening.md)
already names for revisiting this gap.

## Decision

A new, dedicated `secrets` NVS partition (16KB - generous for small
key-value blobs, today one PBKDF2 hash), carved out of `storage`'s own
budget and appended after it in `firmware/partitions.csv`. Appending it
last means no realignment of `ota_0`/`ota_1`'s 64KB boundary is needed,
unlike ADR-0017's original repartition.

`FirmwareSecretStore` now opens this partition explicitly via
`nvs_open_from_partition(kPartitionName, ...)` instead of the plain
`nvs_open()` both stores previously shared; `FirmwareSettingsStore`
continues using the default partition, now passed explicitly
(`NVS_DEFAULT_PART_NAME`) rather than implicitly. `firmware/main/homedeck.cpp`'s
`InitNvs()` initializes both partitions the same way (init, erase-and-
retry once on `ESP_ERR_NVS_NO_FREE_PAGES`/`ESP_ERR_NVS_NEW_VERSION_FOUND`,
then hard-fail), before either store's first use.

This closes the gap structurally: `FirmwareSettingsStore::ListAll()`
physically cannot see anything written through `FirmwareSecretStore`
regardless of `(module_id, key)` collisions, not just for the one
pair the reserved-key guard already knew about. That guard stays in
place on the write path (`Storage::SetSetting` still rejects the exact
admin-password pair) as a second-layer safeguard against a confusing
namespace collision, not because it's still the only thing preventing a
leak.

**Accepted one-time cost:** the K145 reference unit's existing admin
password hash does not carry over to the new partition - first-login
setup runs again after this update. Acceptable now, in early
development, per the project owner's decision; this would need a real
migration path if HomeDeck were shipping to users by the time a change
like this were made.

**This accepted cost extends to `storage` itself, not just the admin
password.** Shrinking `storage` to carve out `secrets`
moves `storage`'s own end boundary. `esp_vfs_fat_spiflash_mount_rw_wl`'s
wear-levelling layer keeps its own bookkeeping near a partition's end,
so an already-provisioned device's existing wear-levelling state is
left misaligned with the new, smaller boundary - every write to
`storage` fails (`FirmwareCacheStore`, FatFs `FR_DENIED`/"no free
cluster", `esp_vfs_fat_info()` reporting 0 bytes free regardless of how
little the volume actually holds), deterministically on every boot, not
as an intermittent race. The fix is the same shape as the admin
password's: a one-time `storage` partition erase (`esptool.py
erase_region`), letting `format_if_mount_failed` rebuild it fresh. No
code-level fix exists, since the corruption is structural to the
repartition itself, not a defect in the storage code - a real migration
path (or a full-flash erase as part of the update step) would be needed
before shipping if a repartition like this were made again.

## Consequences

- `HostSecretStore` needs no equivalent change - it already uses a
  separate directory from `HostSettingsStore` on the host filesystem
  (see its own header comment), so this gap never existed there.
- A future module's own credentials (e.g. Home Assistant's long-lived
  access token, M6) are automatically covered by this same partition
  separation - no per-secret guard to remember to add.
- [ADR-0017](ADR-0017-partition-table.md)'s own table is left as
  originally written; this ADR is the reference for the `secrets`
  partition specifically. It's also the durable fix
  [ADR-0010](ADR-0010-secret-storage.md#status) points to for
  `SecretStore`'s actual firmware backing.
- Any already-provisioned device upgrading across this repartition needs
  its `storage` partition erased once, the same as the admin password
  reset above - see "This accepted cost extends to `storage` itself"
  above for the mechanism and fix.
