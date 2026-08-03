# ADR-0008: Dashboard Widget System

## Status

Accepted

## Context

[dashboard.md](../architecture/dashboard.md) describes the dashboard as a
live, glanceable widget host per [ADR-0004](ADR-0004-ui-philosophy.md).
Three decisions within that scope involved real tradeoffs and rejected
alternatives: how widgets are laid out on screen, how the weather widget
specifically sources its data (since weather has no local source on this
hardware and the project otherwise avoids cloud dependencies), and whether
date/time and battery status belong in that same layout or somewhere else
entirely. This ADR records all three, so the architecture doc can state the
current design without carrying the full rationale inline.

## Decision: Dashboard layout model

**Options:**
- A fixed grid (N columns × M rows, widgets occupy 1×1 or larger cells).
- A freeform/scrollable list of variable-height widget cards.

**Decided: a fixed grid.** It's simpler to implement well for M1/M2,
constrains modules to a predictable widget footprint (easier to keep the
"polished, consumer-quality" bar [CLAUDE.md](../../CLAUDE.md) sets), and doesn't foreclose
moving to a more flexible layout later once there's a real widget catalog
to design against. The freeform option was rejected for now because it's
harder to keep visually coherent across widgets built independently by
unrelated modules, and more design work up front than justified before a
real widget catalog exists. Exact grid dimensions and cell-span rules are
implementation details for M2, not part of this decision.

`DashboardGrid` (`src/ui/dashboard_grid.h`/`.cpp`) is that grid, built on
LVGL's native grid layout (`lv_obj_set_grid_dsc_array`/
`lv_obj_set_grid_cell`). `DashboardGrid::kColumns` (4) — the exact
dimension this decision left open — is provisional, sized with no real
widget catalog to size against yet; rows have no fixed count, growing on
demand as widgets are added. Cell-span is implemented: widgets can occupy more
than one column and/or row (`Widget::ColumnSpan()`/`RowSpan()`,
`src/ui/widget.h`, both defaulting to 1), placed by first-fit scanning
against a per-row occupancy bitset, since multi-row spans need to know
which cells further down are already taken. `Widget` has no live/cached/
offline freshness reporting yet (see
[dashboard.md](../architecture/dashboard.md#data-freshness)).

## Decision: Weather data source

**Context:** [CLAUDE.md](../../CLAUDE.md) lists weather as a Core-provided widget, but weather
data has no local source on this hardware — it's inherently either a
direct third-party API call or data proxied through another integration.

**Options:**
- Always a direct third-party weather API call — available to every user
  regardless of other configuration, but a permanent, unconditional cloud
  dependency baked into Core, at odds with the local-first philosophy (see
  [networking.md](../architecture/networking.md)) for a feature that isn't
  unavoidable.
- Home-Assistant-only — no direct cloud dependency, but contradicts
  [CLAUDE.md](../../CLAUDE.md)'s explicit "Weather services" Core-responsibility listing and
  delays the widget until the HA module (M6).
- A pluggable `WeatherProvider` interface owned by Core, with a
  no-API-key direct provider and an HA-sourced provider as interchangeable,
  user-selectable implementations.

**Decided:** the third option. Core owns the `WeatherProvider` interface
and renders the widget — satisfying [CLAUDE.md](../../CLAUDE.md)'s Core listing as written —
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

## Decision: Status bar vs. dashboard-only widgets

**Context:** [CLAUDE.md](../../CLAUDE.md)'s example widget list treats date/time and battery
status as ordinary dashboard widgets, living in the same fixed grid as
weather, Home Assistant states, and every module-contributed widget. That
scoping means they're only visible while the user happens to be looking at
the dashboard — invisible during remote control, media browsing, or any
other screen, despite being exactly the kind of glanceable, always-relevant
status a handheld device benefits from showing continuously, closer to how
Android, iOS, and most desktop environments treat a status bar than how
they treat a home-screen widget.

**Options:**
- Keep date/time and battery as ordinary dashboard-grid widgets — simple,
  consistent with every other widget's customization model, but invisible
  on every screen except the dashboard.
- A persistent top status bar, shown on every screen (dashboard included),
  sitting outside the widget grid as system-level chrome — architecturally
  the same category as the persistent home affordance
  ([ADR-0004](ADR-0004-ui-philosophy.md#decision-return-home-affordance)),
  not a dashboard widget.

**Decided:** the persistent top bar. It hosts a *compact* date/time and
battery status at minimum. Network status (also listed as an example
widget in [CLAUDE.md](../../CLAUDE.md)) is a natural additional candidate for the bar, but
which widgets beyond date/time/battery belong there is an M2
implementation detail, not decided by this ADR.

This changes what the compact date/time and battery indicators *are*, not
just where they render: they move from customizable dashboard widgets
(subject to the eventual enable/reorder goal — see
[dashboard.md](../architecture/dashboard.md#customization-future)) to fixed
system chrome that a user doesn't remove or reorder, the same treatment as
the home affordance. That's an intentional trade for these two
specifically — a status that's always present is worth more here than
optionality.

This does **not** rule out a separate, optional *large clock* widget also
living in the dashboard grid, the same way Android keeps a compact clock in
its status bar while still offering a larger clock as an optional
home-screen widget. The two serve different purposes — the status bar's
job is guaranteed visibility from any screen; a grid widget's job is a
deliberately prominent presence on the dashboard specifically, for users
who want it — and aren't mutually exclusive. Whether such a widget ships,
and whether the same reasoning extends to an optional large-battery
widget, is ordinary widget-catalog scope for M2 and later, not something
this ADR needs to decide.

The persistent home affordance is a separate, independent piece of chrome
and is **not** being folded into the status bar by this decision. Whether
they should eventually be unified — or whether a broader app-switcher dock
should absorb the home affordance instead, as discussed alongside this
decision — is an open question deferred until there's a real module
catalog to design a dock against (see the M2 widget framework item in
[roadmap.md](../roadmap.md#m2--platform-services-current)). This ADR only settles
where date/time and battery live.

## Consequences

- [dashboard.md](../architecture/dashboard.md) states the resulting design
  (fixed grid for optional/reorderable widgets, pluggable weather provider,
  a separate persistent status bar for date/time and battery) without
  repeating the rejected alternatives.
- The weather widget is opt-in and off by default — a user who enables
  neither provider sees no weather widget, not a broken one.
- The HA module (M6) is a `WeatherProvider` implementation, not the owner
  of the interface — see [modules.md](../architecture/modules.md) for the
  general module/Core boundary this follows.
- The dashboard's widget grid no longer includes date/time or battery —
  its scope narrows to genuinely optional, reorderable content (weather,
  Harmony activity, Uptime Kuma health, Home Assistant states, and future
  module widgets).
- [ui.md](../architecture/ui.md#navigation-model) should describe the
  status bar as a second, independent piece of persistent screen chrome
  alongside the home affordance, not part of the navigation manager itself.
- M1's current hardcoded `DashboardScreen` clock/battery labels already
  show roughly the right information; M2's actual work is relocating them
  into shared status-bar chrome present on every screen, not building a
  grid cell for them.

`StatusBar` (`src/ui/status_bar.h`/`.cpp`) is that relocation, since
joined by a compact Wi-Fi connectivity icon — see
[dashboard.md](../architecture/dashboard.md#status) for the
implementation.
