# ADR-0007: Web Management UI Policies

## Status

Accepted

## Context

[web-ui.md](../architecture/web-ui.md) describes the Web Management UI's
scope and transport. Two decisions within it involved real tradeoffs and
rejected alternatives: how users authenticate to it, and — once initial
Wi-Fi provisioning moved to a separate SoftAP flow (see
[ADR-0006](ADR-0006-networking-discovery-provisioning.md)) — when the admin
credential that authentication depends on actually gets created. This ADR
records both, so the architecture doc can state the current design without
carrying the full rationale inline.

## Decision: Authentication mechanism

**Options:**
- A single local admin password, used for simple session/cookie-based
  login.
- Token-based auth (a long-lived API token issued at setup, used for both
  the Web UI and any future third-party API consumers).

**Decided:** a single local admin password with session-based login.
HomeDeck is a single-user LAN device, and this matches the security bar of
comparable embedded admin UIs (routers, NAS devices) without
over-engineering for a multi-user or third-party-API case that doesn't
exist yet. Token-based auth was rejected as more moving parts for a need
that isn't concrete — revisit if/when programmatic/third-party API access
becomes a real requirement.

## Decision: When the admin password is set

**Context:** the SoftAP captive portal that handles initial Wi-Fi
provisioning (see [ADR-0006](ADR-0006-networking-discovery-provisioning.md))
collects Wi-Fi credentials only. Once that was decided, it left open when
the separate Web UI admin password from the previous decision actually gets
created — the two flows were designed independently and needed to be
reconciled.

**Options:**
- Collected during the SoftAP captive portal too, alongside the Wi-Fi
  credentials, so the device is fully configured by the time it joins the
  LAN.
- First-login-sets-password: the device joins the LAN with no admin
  password set, and the first Web UI visit requires setting one before
  anything else is accessible.

**Decided:** first-login-sets-password, the same pattern routers and NAS
devices use. Folding the admin password into the SoftAP captive portal was
rejected because it expands that flow's scope beyond "just Wi-Fi" and
because a form on a captive portal that shuts down immediately after
provisioning succeeds is a worse place to set a credential than the Web UI
itself — less opportunity to recover if something goes wrong before it's
saved. Until a password is set, the Web UI's only reachable state is the
password-setup screen.

**Accepted risk:** this pattern has a real race condition — between the
device joining the LAN and the intended owner first opening the Web UI,
anyone else who reaches it first (another device on a shared or
compromised network, not necessarily the owner) claims admin control.
This is accepted as consistent with HomeDeck's single-user home-LAN trust
model, the same bar comparable devices (routers, NAS) already operate at.
It is not accepted as a general-purpose stance — if HomeDeck is ever
expected to operate on a less-trusted network, this needs a real fix (e.g.
a short-lived claim token issued to the device that completed SoftAP
provisioning, so only it can complete first-login), not just this note.

## Consequences

- [web-ui.md](../architecture/web-ui.md) states the resulting design (the
  authentication mechanism, the first-login password flow) without
  repeating the rejected alternatives.
- The SoftAP captive portal's scope stays narrow by design — any future
  proposal to add fields to it should be checked against this ADR's
  reasoning first.
