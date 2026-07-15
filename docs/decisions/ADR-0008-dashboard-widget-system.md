# ADR-0008: Dashboard Widget System

## Status

Accepted

## Context

[dashboard.md](../architecture/dashboard.md) describes the dashboard as a
live, glanceable widget host per [ADR-0004](ADR-0004-ui-philosophy.md). Two
decisions within that scope involved real tradeoffs and rejected
alternatives: how widgets are laid out on screen, and how the weather
widget specifically sources its data, since weather has no local source on
this hardware and the project otherwise avoids cloud dependencies. This ADR
records both, so the architecture doc can state the current design without
carrying the full rationale inline.

## Decision: Dashboard layout model

**Options:**
- A fixed grid (N columns × M rows, widgets occupy 1×1 or larger cells).
- A freeform/scrollable list of variable-height widget cards.

**Decided: a fixed grid.** It's simpler to implement well for M1/M2,
constrains modules to a predictable widget footprint (easier to keep the
"polished, consumer-quality" bar CLAUDE.md sets), and doesn't foreclose
moving to a more flexible layout later once there's a real widget catalog
to design against. The freeform option was rejected for now because it's
harder to keep visually coherent across widgets built independently by
unrelated modules, and more design work up front than justified before a
real widget catalog exists. Exact grid dimensions and cell-span rules are
implementation details for M2, not part of this decision.

## Decision: Weather data source

**Context:** CLAUDE.md lists weather as a Core-provided widget, but weather
data has no local source on this hardware — it's inherently either a
direct third-party API call or data proxied through another integration.

**Options:**
- Always a direct third-party weather API call — available to every user
  regardless of other configuration, but a permanent, unconditional cloud
  dependency baked into Core, at odds with the local-first philosophy (see
  [networking.md](../architecture/networking.md)) for a feature that isn't
  unavoidable.
- Home-Assistant-only — no direct cloud dependency, but contradicts
  CLAUDE.md's explicit "Weather services" Core-responsibility listing and
  delays the widget until the HA module (M6).
- A pluggable `WeatherProvider` interface owned by Core, with a
  no-API-key direct provider and an HA-sourced provider as interchangeable,
  user-selectable implementations.

**Decided:** the third option. Core owns the `WeatherProvider` interface
and renders the widget — satisfying CLAUDE.md's Core listing as written —
but the data source is pluggable and user-selectable in the Web Management
UI, with no default enabled:

- **Direct provider:** Open-Meteo, chosen specifically because it requires
  no API key or account, minimizing what a "direct cloud" option actually
  commits the user to. Small enough (one API call, no discovery/auth/
  lifecycle) to live in Core rather than as a full module. Requires the
  user to manually enter a location (address or coordinates) — not
  IP-geolocation, which would itself be a silent cloud lookup and undercut
  the point of making this opt-in.
- **Home Assistant provider:** supplied by the HA module (M6) through the
  same interface once configured, reusing whatever weather source the user
  already has in HA.

Because the direct provider doesn't depend on any module, the weather
widget can ship in M2 (once the widget framework exists) rather than
waiting for HA at M6 — resolving the "always direct" vs. "HA-only" options'
respective downsides (mandatory cloud dependency vs. delayed availability)
without accepting either.

## Consequences

- [dashboard.md](../architecture/dashboard.md) states the resulting design
  (fixed grid, pluggable weather provider) without repeating the rejected
  alternatives.
- The weather widget is opt-in and off by default — a user who enables
  neither provider sees no weather widget, not a broken one.
- The HA module (M6) is a `WeatherProvider` implementation, not the owner
  of the interface — see [modules.md](../architecture/modules.md) for the
  general module/Core boundary this follows.
