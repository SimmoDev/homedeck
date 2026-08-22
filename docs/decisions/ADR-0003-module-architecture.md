# ADR-0003: Module Architecture

## Status

Accepted (contract shape) — exact interfaces were finalized during M3
implementation, not defined here; see
[modules.md's Status section](../architecture/modules.md#status) for the
finalized `Module` contract (`src/core/module.h`) and Harmony's own
implementation of it. The Known External Risk section below's XMPP
protocol framing, and its "discovering and authenticating against an
already-configured hub" characterization, are both superseded by
[ADR-0029](ADR-0029-harmony-local-protocol.md), which confirms the
reference hub instead speaks a local WebSocket/JSON API on port 8088 with
no authentication step of any kind — a local connection, not a discovery-
and-authentication flow. That section's project-context conclusion
(scoped to local control of an already-paired hub) stands unchanged.

## Context

[CLAUDE.md](../../CLAUDE.md) requires that integrations (Harmony, Kodi, Uptime Kuma, Home
Assistant, and future ones) be implemented internally as isolated modules
that present themselves to the user as Apps, that Core know as little as
possible about individual modules, and that modules never communicate with
each other directly. This ADR records the shape of that contract at an
architectural level. It intentionally does not define C++ interfaces,
method signatures, or protocol details — [CLAUDE.md](../../CLAUDE.md) instructs against making
assumptions about APIs before the first module is actually built, since that
tends to produce speculative abstractions that don't fit real
implementation needs.

## Decision

### Module boundary

A module is a self-contained unit that:

- Registers itself with Core at startup (screens/routes, dashboard widgets,
  settings pages, background tasks, API endpoints) rather than Core knowing
  about it by name.
- Owns all logic specific to the external system it integrates with
  (Harmony Hub protocol, Kodi JSON-RPC, Uptime Kuma API, Home Assistant
  API/WebSocket).
- Publishes events to the shared event bus to communicate state changes
  (e.g. "Harmony activity changed") and never calls into another module's
  code directly.
- Can be enabled/disabled and started/stopped independently without Core or
  other modules needing special-case handling.

### Why events instead of direct module-to-module calls

Direct calls between modules (e.g. Harmony module calling into a
hypothetical notification-specific method on another module) would
reintroduce the tight coupling [CLAUDE.md](../../CLAUDE.md) explicitly warns against, and would
make modules impossible to test or ship independently. The event
bus + shared Core services (notifications, storage, widgets) are the only
sanctioned communication paths between a module and the rest of the system.

This is a behavioral/contractual boundary, not a memory-safety one — see
[modules.md](../architecture/modules.md#isolation-and-independent-operation)
for why that distinction matters on hardware where FreeRTOS gives every
task a shared address space rather than MMU-based process isolation.

### Why Core stays thin

Core provides *generic* services (lifecycle, navigation, storage, event bus,
notifications, widget registry, networking, diagnostics) that any module can
use, but Core must not contain Harmony-specific, Kodi-specific, etc. logic.
The test for whether something belongs in Core or in a module: "would a
future module unrelated to this integration need this capability?" If yes,
it's a Core service; if no, it belongs in the module.

### Lifecycle expectations

Every module is expected to support a consistent lifecycle managed by Core
(init → start → stop → teardown), so Core can enable/disable modules
uniformly (e.g. from the Web Management UI) without bespoke per-module
startup code. Background tasks a module runs must respect this lifecycle —
a stopped module should not continue polling in the background, per the
background task requirements in [CLAUDE.md](../../CLAUDE.md).

## Consequences

- The first module implementation (Harmony, M3) effectively becomes the
  reference implementation of this contract. Expect the exact interface
  shape to be refined once real requirements (Harmony hub discovery,
  authentication, activity/device modeling) are worked through in practice.
- Kodi, Uptime Kuma, and Home Assistant modules must be evaluated against
  whether they fit the contract established by Harmony without needing
  Core changes. If a second module needs a Core change to fit, that's a
  signal the Harmony-derived contract was too narrow and needs revisiting
  before a third module is built.
- Documentation for the finalized module interface belongs in
  [architecture/modules.md](../architecture/modules.md) once implemented —
  this ADR records the decision to have a contract and its shape, not the
  contract's code.

## Known External Risk: Harmony Hub Local Control

Not an architectural decision, but a risk worth recording here since it
directly affects the reference module: Logitech has been winding down
Harmony's cloud services since announcing end-of-life in 2021. Historically,
Harmony Hub setup and pairing depended on Logitech's cloud, while day-to-day
control of an already-paired hub happens over a local XMPP-based protocol on
the LAN (reverse-engineered by the community, e.g. via `pyharmony` and the
Home Assistant Harmony integration). It is not confirmed whether new hub
pairing is still possible without Logitech's cloud, or only continued
control of already-configured hubs.

**Project context:** the project owner already has a Harmony Hub, paired
and working via Logitech's app. This narrows the M3 scope meaningfully —
HomeDeck's Harmony module needs to reliably support local control of an
*already-paired* hub (the XMPP-based local protocol), which is the scenario
that matters for this project's own reference hardware and for the
realistic majority of prospective users (existing Harmony owners, not new
buyers, since Harmony hardware is no longer sold new). "Hub discovery and
authentication" in the M3 scope (see
[roadmap.md](../roadmap.md#m3--harmony-current)) should be understood as
discovering and authenticating against an already-configured hub on the
LAN, not performing Logitech's original first-time cloud pairing flow.
Whether first-time cloud pairing is still possible at all remains
unconfirmed and is out of scope to solve unless it turns out to matter for
a specific user's setup.
