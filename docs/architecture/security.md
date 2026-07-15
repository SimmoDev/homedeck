# Security

CLAUDE.md requires security to be "considered from the beginning," with
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

## Requirement: avoid insecure secret storage

Wi-Fi credentials and settings are protected via NVS encryption using the
HMAC-peripheral-based key scheme (not ESP-IDF's default flash-encryption-
based scheme, which was considered and found to reintroduce the exact
development cost this decision avoids); the admin password is hashed
(never stored reversibly) as defense in depth on top of that. See
[ADR-0010](../decisions/ADR-0010-secret-storage.md) for the confirmed
scheme choice and the eFuse provisioning step it requires during
manufacturing/first-flash.

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

**Known gap, deliberately deferred, not silently absent.** ESP-IDF's A/B
partition scheme (see [power-management.md](power-management.md)) protects
against *boot-time corruption* from an interrupted OTA write — it does not
verify that an OTA image actually came from the project itself. Today, the
only barrier to pushing an arbitrary firmware image is the Web UI's admin
authentication gating the OTA endpoint (see
[web-ui.md](web-ui.md#security)) — so the realistic exposure is an
authenticated LAN attacker, not an open one, but it is still a real gap
against a first-class CLAUDE.md feature.

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

Not yet implemented — this document describes required behavior for M2
(NVS encryption, admin auth, input validation) and a recorded gap for later
(OTA signing), consistent with [core.md](core.md) and
[web-ui.md](web-ui.md).
