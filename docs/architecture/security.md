# Security

[CLAUDE.md](../../CLAUDE.md) requires security to be "considered from the beginning," with
five explicit requirements. This document is the cross-cutting reference
for how each is addressed — most of the actual mechanisms live in the
docs/ADRs that own the relevant subsystem; this page exists so "is security
covered" has one place to check rather than needing to be pieced together.

## Requirement: do not expose unauthenticated management controls by default

Addressed in [web-ui.md](web-ui.md#security) and
[ADR-0007](../decisions/ADR-0007-web-management-ui-policies.md): the Web
Management UI requires a local admin password after first login; the only
unauthenticated surface is the SoftAP captive portal used for initial Wi-Fi
provisioning, which is narrowly scoped to collecting Wi-Fi credentials and
shuts down once provisioning succeeds.

## Requirement: protect configuration changes

Addressed the same way — configuration-changing endpoints require
authentication once the device is on the LAN. See [Requirement: validate
API input](#requirement-validate-api-input) below for the related but
distinct question of validating *what* an authenticated request contains.

Login attempts are rate-limited: `AdminAuthService` locks out further
login attempts for a cooldown period once a small number of consecutive
failures land, closing the "guess the password unboundedly" gap a bare
password check alone would leave open. State-changing endpoints rely on
the session cookie's `SameSite=Strict` attribute as their CSRF
mitigation — a deliberate choice, not an oversight — rather than a
separate per-request CSRF token, since it blocks the cookie from being
sent on cross-site requests (including top-level GET navigation)
without the extra token-plumbing cost. Revisit this if `SameSite` is
ever relaxed for an unrelated reason (e.g. embedding) — that change
would reopen this gap and deserves its own security review at that
point.

## Requirement: avoid insecure secret storage

The admin password is hashed (never stored reversibly) regardless of the
storage tier — this holds from the start, not staged. NVS encryption
itself follows a staged security model
([ADR-0018](../decisions/ADR-0018-staged-security-hardening.md)): the
current (Development) tier stores NVS-resident secrets in plaintext, by
design, since flashing/debugging convenience outweighs a threat this
project's current secret surface doesn't yet justify hardening against.
The Standard tier activates NVS encryption via the HMAC-peripheral key
scheme ADR-0010 already chose (not ESP-IDF's default flash-encryption-
based scheme, which was considered and rejected for reintroducing real
re-flashing development cost) once a real module credential exists to
justify its one-time, irreversible eFuse provisioning step. See
[ADR-0010](../decisions/ADR-0010-secret-storage.md) for the scheme choice
and [ADR-0018](../decisions/ADR-0018-staged-security-hardening.md) for
the staging decision. Wi-Fi credentials are outside this requirement's
scope entirely — they live on the C6 co-processor's own flash, not this
project's NVS partition (see
[hardware.md](hardware.md#wi-fi-bring-up)).

## Requirement: validate API input

**Principle decided now, mechanism deferred.** All API input (REST request
bodies, WebSocket messages, query parameters) must be validated before use
— this is a hard requirement, not optional. The specific validation
mechanism (centralized schema validation at Core's HTTP server layer vs.
per-endpoint validation) is deferred until M2, when the first real API
endpoints are built, consistent with the project's established stance
against designing mechanisms ahead of a real consumer (see
[ADR-0003](../decisions/ADR-0003-module-architecture.md)). Deferring the
mechanism is not the same as deferring the requirement — no endpoint ships
without input validation, regardless of which mechanism M2 lands on.

## Requirement: minimise external dependencies

Addressed in spirit throughout [ADR-0002](../decisions/ADR-0002-technology-stack.md)
— every dependency choice (nlohmann::json, civetweb, Svelte, GoogleTest) is
justified individually rather than added by default, and several
alternatives were rejected specifically for adding an unnecessary
dependency (e.g. the camera-QR provisioning path in
[ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md)).

## OTA image integrity

**Known gap, deliberately deferred, not silently absent.** The real A/B
partition scheme is in place — `ota_0`/`ota_1` app partitions plus
bootloader app-rollback (see
[ADR-0017](../decisions/ADR-0017-partition-table.md)) — and protects
against *boot-time corruption* from an interrupted OTA write. It does not
verify that an OTA image actually came from the project itself. Today, the
only barrier to pushing an arbitrary firmware image is the Web UI's admin
authentication gating the OTA endpoint (see
[web-ui.md](web-ui.md#security)) — so the realistic exposure is an
authenticated LAN attacker, not an open one, but it is still a real gap
against a first-class [CLAUDE.md](../../CLAUDE.md) feature.

ESP-IDF supports signed app image verification independent of full secure
boot, which would close this. It was deliberately not adopted now because
it requires establishing and protecting a signing key, and complicates the
open-source build story — a contributor building their own firmware
wouldn't have the project's private signing key unless signing is
conditionally disabled for self-built images, which itself needs careful
design to avoid becoming a bypass. This deserves a deliberate decision
before any release with a broader trust boundary than "single-user home
LAN, gated by admin auth" — not a default either way.

## Status

**Admin auth is implemented** — see [web-ui.md](web-ui.md#status) for
`AdminAuthService`'s design and verification status, including its
password hashing (satisfying the "avoid insecure secret storage"
requirement's hashing half) and the `RequireAuth()` gate (satisfying
"do not expose unauthenticated management controls by default" and
"protect configuration changes" for whatever it wraps). Its two
endpoints that take a request body (`setup`/`login`) validate input
today — a minimum password length and well-formed JSON are both
checked, satisfying "validate API input" for those two. The rest of the
API surface (diagnostics, OTA, settings, weather, Wi-Fi reset) now
exists too, and each route validates its own request body
independently at its own handler — the mechanism decision this
requirement calls out (centralized vs. per-endpoint) landed as
per-endpoint by default, not a deliberate centralized design.

**NVS encryption is deliberately deferred, not open** — see
[ADR-0018](../decisions/ADR-0018-staged-security-hardening.md) for the
staged model that places it in a future Standard tier rather than the
current Development tier. Everything else this document describes (OTA
signing) remains the recorded, deliberately deferred gap it already was.
