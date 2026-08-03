# ADR-0018: Staged Security Hardening Model

## Status

Accepted

## Context

[ADR-0010](ADR-0010-secret-storage.md) decided NVS encryption via the
HMAC-peripheral scheme as M2 scope, alongside hashing the admin password.
Before implementing it, a review of the actual threat model against the
project's current state found that timing unjustified, for reasons
specific to how this scheme actually works, not a general objection to
encryption:

- **What it protects today is minimal.** NVS currently holds exactly one
  secret — the admin password hash, already PBKDF2-SHA256 hashed. Module
  credentials (Harmony hub auth, a Home Assistant long-lived token, and
  similar) don't exist yet; they arrive with M3 onward.
- **What it protects against is narrower than "encrypted storage"
  implies.** The HMAC scheme derives its key from the eFuse-programmed
  HMAC peripheral at runtime — any firmware running on that exact chip can
  ask the peripheral for the same key. Without Secure Boot, an attacker
  with physical access can flash alternate firmware and read NVS out
  through the normal decrypt path; the scheme's real guarantee is against
  reading the flash chip out-of-band (desoldering it, reading it with an
  SPI programmer), not against a reflashed device.
- **Activating it is a one-time, irreversible, per-device hardware
  action**, not a config flag. ESP-IDF's HMAC scheme burns the eFuse key
  automatically and silently the first time `nvs_flash_init()` runs on a
  build with the relevant Kconfig options set — there's no interactive
  confirmation point that early in boot (before Wi-Fi, before any UI). On
  an open-source project where contributors flash their own hardware,
  that's a real device-state change without a deliberate per-contributor
  decision behind it, not just an implementation detail.

This ADR does not reverse ADR-0010's scheme choice — HMAC-peripheral over
the flash-encryption-based scheme remains correct for the reasons already
given there. It changes when that scheme activates, and situates the
decision within a broader model covering HomeDeck's security posture as a
whole, not NVS encryption specifically.

## Decision

**Three tiers, activated when their cost is actually justified by what
they protect:**

### Development

Plaintext NVS. No Secure Boot, no flash encryption, no eFuse programming.
The admin password stays PBKDF2-SHA256 hashed regardless — that's
orthogonal to NVS encryption and already in place (see
[ADR-0010](ADR-0010-secret-storage.md)). Maximum flashing/debugging
convenience, zero irreversible state on any development unit.

**This is the current tier**, covering M2 onward until a real module
credential first exists to protect — as early as M3 (Harmony hub
credentials), no later than M6 (Home Assistant's long-lived token, the
last milestone scoped through M6 that's guaranteed to need one).

### Standard

HMAC-peripheral NVS encryption, per ADR-0010's scheme choice — still no
Secure Boot or flash encryption, preserving the re-flashing workflow that
motivated rejecting the flash-encryption-based scheme in the first place.
Activated once a real module credential (not just the admin password
hash) is actually being stored, at which point flash-extraction of a
stolen device becomes a threat worth this tier's one-time hardware cost.

### Hardened

Secure Boot + flash encryption + HMAC-peripheral NVS encryption as a
coordinated bundle, with real per-unit factory key provisioning — the
tier where NVS encryption's protection actually closes, since Secure Boot
stops an attacker from just flashing a different image to read the same
keys. Relevant only if HomeDeck becomes a manufactured product with real
distribution; not a milestone today, recorded here so the option exists
on the record rather than being reinvented later.

## Consequences

- [ADR-0010](ADR-0010-secret-storage.md)'s "Implementation note" section
  is superseded by this ADR for *when* the HMAC scheme activates; its
  scheme choice and rationale stand unchanged.
- [docs/roadmap.md](../roadmap.md) tracks NVS encryption as Standard-tier
  work, not M2 scope — moved out of the M2 checklist to a future
  hardening pass.
- Wi-Fi credentials are unaffected by any tier here: they live on the C6
  co-processor's own flash, outside this project's NVS partition entirely
  (see [hardware.md](../architecture/hardware.md#wi-fi-bring-up)) — a
  scope correction against ADR-0010's original Consequences section,
  which named them alongside the admin password hash.
- A future Standard-tier pass is its own implementation work, with its
  own eFuse-burn verification, not something to fold into an unrelated
  change
  — it gets its own review and explicit go-ahead when undertaken, per
  [CLAUDE.md](../../CLAUDE.md)'s standing requirement to confirm irreversible actions before
  taking them.
