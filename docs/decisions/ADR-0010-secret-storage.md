# ADR-0010: Secret Storage

## Status

Accepted — **when** the HMAC-peripheral NVS encryption scheme activates
is decided separately by
[ADR-0018](ADR-0018-staged-security-hardening.md); the scheme choice and
`SecretStore` interface decisions below stand as originally accepted. On
firmware, the Decision section's claim that `SecretStore` shares
`SettingsStore`'s physical NVS storage is superseded by
[ADR-0027](ADR-0027-secret-store-partition-separation.md), which gives it
a dedicated `secrets` partition instead — the seam this ADR decided to
create is what made that later change a backing-implementation swap
rather than an interface change.

## Context

[CLAUDE.md](../../CLAUDE.md)'s Security section requires HomeDeck to "avoid insecure secret
storage." The Web UI admin password (see
[ADR-0007](ADR-0007-web-management-ui-policies.md)) is the first secret
this project stores; module credentials (Harmony hub auth, a Home
Assistant long-lived token, and similar) follow from M3 onward. Wi-Fi
credentials, collected during SoftAP provisioning (see
[ADR-0006](ADR-0006-networking-discovery-provisioning.md)), are not in
scope here — they're persisted by `esp_wifi_remote` on the C6
co-processor's own flash, outside this project's NVS partition entirely
(see [hardware.md](../architecture/hardware.md#wi-fi-bring-up)). Neither
[core.md](../architecture/core.md)'s Storage responsibility nor any other
document specified how NVS-resident secrets are protected at rest before
this ADR — they were being persisted the same way as any other
configuration value.

## Decision

**Options:**
- Full flash encryption — encrypts the entire flash contents, including
  application code, not just secrets.
- NVS encryption — ESP-IDF's targeted feature for encrypting the NVS
  partition specifically, where secrets are stored.
- No encryption — plaintext NVS, deferred to a later milestone.

**Decided:** NVS encryption for NVS-resident secrets, plus hashing
(salted) the admin password before storage regardless of encryption — the
password itself is never stored reversibly, as defense in depth on top of
the encryption.

Full flash encryption was rejected for now specifically because of its
development cost: once enabled it complicates the re-flashing workflow
used throughout M1/M2 iteration (production builds typically need a
separate unencrypted development flow), which is a real ongoing cost for a
project still establishing its build/bring-up process — more than a Wi-Fi
password and one password hash actually require. NVS encryption targets
the same secrets without that cost. Plaintext storage was rejected outright
as a direct violation of an explicit [CLAUDE.md](../../CLAUDE.md) requirement, not a genuine
option.

**Which NVS encryption scheme, specifically — confirmed against current
ESP-IDF documentation (not assumed):** ESP-IDF actually offers two distinct
schemes for protecting NVS encryption keys, and they are not
interchangeable for this decision's purposes:

- The **flash-encryption-based scheme** (ESP-IDF's default/most commonly
  documented one) stores NVS encryption keys in a dedicated key partition
  protected by flash encryption — "enabling Flash Encryption becomes a
  prerequisite for NVS encryption here," per Espressif's own documentation.
  Using this scheme would silently reintroduce the exact development cost
  this ADR set out to avoid.
- The **HMAC-peripheral-based scheme**, confirmed available on the
  ESP32-P4, derives encryption keys at runtime from an HMAC key programmed
  into eFuse (`ESP_EFUSE_KEY_PURPOSE_HMAC_UP`,
  `CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID`) — the key is never stored on flash at
  all, encrypted or otherwise, and this scheme explicitly does not require
  flash encryption.

**Decided: the HMAC-peripheral-based scheme specifically**, not ESP-IDF's
default flash-encryption-based one. It delivers what "NVS encryption"
means for this ADR's purposes without depending on flash encryption: the
key is never flash-resident even in encrypted form, derived at runtime
from silicon rather than read from storage.

**Concrete implementation requirement this surfaces:** the HMAC key must
be programmed into eFuse before this scheme is active. eFuses are
one-time-programmable — this is an irreversible, per-device provisioning
step, not a config flag flipped at will. See
[ADR-0018](ADR-0018-staged-security-hardening.md) for when that step
happens.

Sources: [ESP-IDF NVS Encryption docs (ESP32-P4)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/storage/nvs_encryption.html).

## Decision: secret storage interface

**Context:** `AdminAuthService` currently stores the password hash
through `Storage::SetSetting()` — the same call path any module uses for
ordinary configuration, distinguished only by which key string is passed.
Nothing structurally marks that value as a secret. [ADR-0012](ADR-0012-storage-tiers.md#decision-backup-delivery)
already commits to a Web UI backup export sweeping "config + module
settings" — without a structural distinction, backup code has no signal
telling it to exclude the password hash, or any future module credential,
from that export. This is the same problem
[ADR-0012](ADR-0012-storage-tiers.md#decision-storage-namespacing)
already solved for per-module namespacing: a correctness requirement that
depends on every call site remembering a convention is exactly what
should be structurally enforced instead.

**Decided:** a `SecretStore` interface, distinct from `SettingsStore`,
mirroring its shape (`Set`/`Get`/`Erase`) but semantically marking
anything routed through it as excluded from config export and any future
diagnostics dump. In the current (Development) security tier it's backed
by the same plain NVS storage `SettingsStore` uses — this decision is
about creating the seam, not about encrypting anything, which stays
governed by [ADR-0018](ADR-0018-staged-security-hardening.md). When the
Standard tier activates NVS encryption, `SecretStore`'s backing
implementation is the one place that changes; callers (`AdminAuthService`
today, module credential storage from M3 on) don't.

`SecretStore` is implemented on both targets — see
[core.md](../architecture/core.md#status) for the implementation.

## Consequences

- The admin password hashing scheme (algorithm, salt handling) is an
  implementation detail for M2, not fixed by this ADR — any standard salted
  hash (e.g. bcrypt-family) satisfies the requirement.
- Full flash encryption remains available to revisit for a production/
  release build once the development workflow cost is no longer a factor —
  this ADR does not rule it out permanently, only for now.
- This ADR does not cover OTA image signing/verification, a related but
  distinct gap — see [security.md](../architecture/security.md#ota-image-integrity).
- **When** the HMAC scheme activates, and the eFuse provisioning step that
  requires, is decided by [ADR-0018](ADR-0018-staged-security-hardening.md),
  not by this ADR — this ADR fixes the scheme choice, ADR-0018 fixes the
  timing.
- Wi-Fi credentials live on the C6 co-processor's own flash, outside this
  project's NVS partition entirely (see
  [hardware.md](../architecture/hardware.md#wi-fi-bring-up)) — this ADR's
  scheme covers NVS-resident secrets (currently, the admin password hash),
  not Wi-Fi credentials.
- `AdminAuthService` routes the admin password hash through `SecretStore`
  (see [Decision: secret storage
  interface](#decision-secret-storage-interface) above), not
  `Storage::SetSetting()` — any future module credential storage does the
  same.

## Implementation note: NVS encryption timing

The admin password hash (`AdminAuthService`, see
[web-ui.md](../architecture/web-ui.md#status)) is PBKDF2-SHA256 hashed
before it reaches Storage, but stored in the plain, unencrypted NVS tier.
This is deliberate, not outstanding: [ADR-0018](ADR-0018-staged-security-hardening.md)
places NVS encryption in a Standard security tier, activated once a real
module credential exists to justify the one-time, irreversible eFuse
burn the HMAC scheme requires — not in the current (Development) tier.
