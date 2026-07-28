# Apps and Modules

Internally, integrations are implemented as **modules**. To the user, they
appear as **Apps**. This document describes the module contract at an
architectural level; see
[ADR-0003](../decisions/ADR-0003-module-architecture.md) for the reasoning
behind it.

## What a module may provide

- Screens (registered with Core's navigation, not self-managed)
- Dashboard widgets (registered with Core's widget system, see
  [dashboard.md](dashboard.md))
- Configuration pages (surfaced in the Web Management UI)
- Background tasks (scheduled/lifecycle-managed by Core via the [Core
  Concurrency Abstraction](../decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction),
  not FreeRTOS/host APIs directly — see
  [power-management.md](power-management.md) for the power constraints)
- Events (published to the shared event bus)
- Settings (persisted through Core's configuration service)
- API endpoints (exposed through Core's networking layer)
- Notifications (published to Core's notification service, with an urgency
  level that drives presentation — see
  [power-management.md](power-management.md#notifications-during-sleeping);
  a module's normal background task is sufficient for this, since Core
  keeps running through `Sleeping` the same as `Idle`, no special
  Sleeping-specific hook needed)

## Registration, not coupling

A module never wires itself into the UI, navigation, or dashboard by
directly instantiating or referencing those systems' internals. It
registers: "here is a screen, reachable from this route," "here is a widget,
here is how it renders," "here is a settings page." Core owns the actual
placement, navigation, and rendering orchestration. This is what allows
Core to know as little as possible about individual modules — Core deals in
registered screens/widgets/routes, not in "the Harmony module."

## Modules do not talk to each other

If Kodi needs to react to something Harmony did (e.g. pausing playback when
an activity changes), that happens through the event bus: Harmony publishes
an event describing what changed, and any module — including Kodi — may
subscribe to it. Neither module has a reference to the other, and neither
knows the other exists. See
[overview.md](overview.md#event-driven-design) for the general pattern.

## Isolation and independent operation

Each module should be enable-able, disable-able, and independently
start/stop-able. A user who doesn't use Kodi should be able to disable that
module entirely — no other module or Core behavior should depend on it
being present. This also means modules must fail independently: a Harmony
Hub being unreachable must not degrade Kodi or Home Assistant functionality.

**What "isolation" means here, precisely:** behavioral and contractual —
enforced by the lifecycle discipline and event-bus-only communication
described above, not by memory protection. FreeRTOS on this hardware gives
every task a shared, flat address space; there is no MMU-based process
boundary between modules the way there would be on a desktop OS. A memory-
safety bug in one module (e.g. a buffer overflow while parsing a malformed
response from its own external service) is not architecturally contained
from corrupting Core or another module's memory — the architecture cannot
promise that, and building real sandboxing (were it even feasible on this
hardware) would be a disproportionate undertaking for this project's
scope. The practical consequence: each module is responsible for
defensively parsing and bounds-checking data from its own external
service, since that's ordinary secure-coding discipline, not something
the module boundary does for it.

## Planned modules

| Module | Milestone | Status |
|---|---|---|
| Harmony Hub | M3 | Not started — see [roadmap.md](../roadmap.md) |
| Kodi | M4 | Not started |
| Uptime Kuma | M5 | Not started |
| Home Assistant | M6 | Not started |
| MQTT, Jellyfin, Plex, Spotify, Prometheus, Grafana, ESPHome, Shelly | Future | Not scoped |

Each module-specific architecture document (e.g. a future
`docs/architecture/harmony.md`) should be written once that module's design
is actually being implemented, not speculatively ahead of it — see the
scope-control guidance in CLAUDE.md.

## Status

The concrete module interface (exact base class/lifecycle methods, exact
registration API) is intentionally undefined here. CLAUDE.md instructs
against making assumptions about APIs before implementation; the interface
will be defined when Harmony (the reference module) is built in M3, and this
document updated to reflect the real contract rather than a speculative one.
