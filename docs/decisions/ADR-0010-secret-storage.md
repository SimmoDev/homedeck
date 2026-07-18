# ADR-0010: Secret Storage

## Status

Accepted

## Context

CLAUDE.md's Security section requires HomeDeck to "avoid insecure secret
storage." Two secrets exist from M2 onward: the Wi-Fi credentials collected
during SoftAP provisioning (see
[ADR-0006](ADR-0006-networking-discovery-provisioning.md)) and the Web UI
admin password (see [ADR-0007](ADR-0007-web-management-ui-policies.md)).
Neither [core.md](../architecture/core.md)'s Storage responsibility nor any
other document specified how these are protected at rest before this ADR —
they were being persisted the same way as any other configuration value.

## Decision

**Options:**
- Full flash encryption — encrypts the entire flash contents, including
  application code, not just secrets.
- NVS encryption — ESP-IDF's targeted feature for encrypting the NVS
  partition specifically, where Wi-Fi credentials and settings are stored.
- No encryption — plaintext NVS, deferred to a later milestone.

**Decided:** NVS encryption for Wi-Fi credentials and settings, plus
hashing (salted) the admin password before storage regardless of
encryption — the password itself is never stored reversibly, as defense in
depth on top of the encryption.

Full flash encryption was rejected for now specifically because of its
development cost: once enabled it complicates the re-flashing workflow
used throughout M1/M2 iteration (production builds typically need a
separate unencrypted development flow), which is a real ongoing cost for a
project still establishing its build/bring-up process — more than a Wi-Fi
password and one password hash actually require. NVS encryption targets
the same secrets without that cost. Plaintext storage was rejected outright
as a direct violation of an explicit CLAUDE.md requirement, not a genuine
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

**New concrete implementation requirement this surfaces:** the HMAC key
must be programmed into eFuse during manufacturing/first-flash. eFuses are
one-time-programmable — this is an irreversible, per-device provisioning
step, not a config flag flipped at will. This needs to be a defined step
in the M1/M2 flashing/provisioning process, not an afterthought discovered
when someone tries to enable NVS encryption on a unit that was never
provisioned for it.

Sources: [ESP-IDF NVS Encryption docs (ESP32-P4)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/storage/nvs_encryption.html).

## Consequences

- Core's Storage service (see
  [core.md](../architecture/core.md#responsibilities)) must use NVS
  encryption for the credential-bearing partition from the point Wi-Fi
  provisioning and Web UI auth are implemented in M2 — this is not
  optional or deferrable the way exact power thresholds are.
- The admin password hashing scheme (algorithm, salt handling) is an
  implementation detail for M2, not fixed by this ADR — any standard salted
  hash (e.g. bcrypt-family) satisfies the requirement.
- Full flash encryption remains available to revisit for a production/
  release build once the development workflow cost is no longer a factor —
  this ADR does not rule it out permanently, only for now.
- This ADR does not cover OTA image signing/verification, a related but
  distinct gap — see [security.md](../architecture/security.md#ota-image-integrity).
- The eFuse provisioning requirement above is a hard M1/M2 process
  dependency, not an implementation detail to discover later.

## Implementation note: NVS encryption split from the Web UI auth pass

The admin password hash (`AdminAuthService`, see
[web-ui.md](../architecture/web-ui.md#status)) is PBKDF2-SHA256 hashed
before it reaches Storage, but stored in the plain, unencrypted NVS tier
— the HMAC-peripheral encryption scheme this ADR decides on, including
the one-time, irreversible eFuse key burn it requires, is not yet
enabled. This is an outstanding requirement, not a reversal of the
decision above, and applies equally to the Wi-Fi credentials
([ADR-0006](ADR-0006-networking-discovery-provisioning.md)). Enabling it
is tracked as its own dedicated, separately-verified follow-up.
