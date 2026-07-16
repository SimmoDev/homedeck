# Dashboard

The dashboard is HomeDeck's default home screen. Per
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md#dashboard-as-home-not-a-launcher-grid),
it is a live, glanceable view of information, not a static grid of app
icons — the goal is for HomeDeck to feel like a living-room command centre,
not just a remote launcher. See
[ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md) for the
tradeoffs and rejected alternatives behind the decisions referenced below.

## Widget system

Widgets are the unit of dashboard content. Core owns the dashboard layout
and rendering host; modules (and Core itself, for things like weather)
contribute widgets through a standard interface rather than being
hardcoded into the dashboard. Widgets are arranged in a fixed grid — see
[ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md#decision-dashboard-layout-model)
for why a fixed grid was chosen over a freeform layout.

A compact date/time and battery status are *not* dashboard widgets,
despite being listed as example widgets in CLAUDE.md — see [Status
bar](#status-bar) below for why they live in persistent screen chrome
instead. This doesn't rule out a separate, optional *large clock* widget
also living in the grid — the same way Android pairs a compact status-bar
clock with an optional larger clock widget on the home screen — that's
ordinary widget-catalog scope, not decided here.

Example grid widgets:

- Weather (Core — see [Weather source](#weather-source) below)
- Network status (Core)
- Current Harmony activity (Harmony module)
- Uptime Kuma service health (Uptime Kuma module)
- Home Assistant states (Home Assistant module)
- Large clock (Core, optional — distinct from the always-on status bar
  time, see [Status bar](#status-bar) below)

Because widgets are contributed through a standard interface, a module can
ship a new widget without any Core change, and the dashboard doesn't need
to know what kind of module produced a given widget.

## Status bar

A compact date/time and battery status are shown in a persistent status bar
present on every screen, not just the dashboard — closer to how Android,
iOS, and desktop environments treat a status bar than how they treat a
home-screen widget. Unlike grid widgets, the status bar isn't
user-customizable (no enable/disable, no reordering); it's fixed system
chrome, the same treatment as the [persistent home
affordance](ui.md#navigation-model). Network status is a natural candidate
to also live here rather than in the grid, but that isn't decided — see
[ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets)
for the full rationale and what's still open (including whether the status
bar and the home affordance should eventually be a single, unified piece
of chrome).

## Weather source

Core owns a pluggable `WeatherProvider` interface and renders the widget;
the data source is user-selectable in the Web Management UI, with no
default enabled:

- **Direct provider** — Open-Meteo (no API key required), with the user
  entering a location manually. Available from M2 onward, independent of
  any module.
- **Home Assistant provider** — supplied by the HA module (M6) through the
  same interface, reusing whatever weather source the user already has in
  HA.

See
[ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md#decision-weather-data-source)
for why this two-provider shape was chosen over always calling a weather
API directly or making weather Home-Assistant-only.

## Data freshness

Widgets must be able to represent, and the dashboard must be able to
visually distinguish, at minimum:

- Live data
- Cached data
- Offline/unavailable state

This follows directly from the offline-behavior requirements in CLAUDE.md
(see also [networking.md](networking.md#offline-behaviour)) — a widget
showing stale Home Assistant state during a Wi-Fi outage must not look
identical to one showing current state.

## Customization (future)

CLAUDE.md establishes an eventual goal of user-customizable:

- Enabled widgets
- Widget order
- Layout
- Favourite actions

This is explicitly a later-stage goal (see [roadmap.md](../roadmap.md),
M7 — Polish) rather than an M1/M2 requirement. The widget interface should
be designed so this is *possible* later (e.g. widgets shouldn't assume a
fixed position or a fixed set of siblings), but building a full
drag-to-reorder customization UI is out of scope until the dashboard has
enough real widgets to make customization meaningful.

## Status

The initial dashboard shell exists and runs in the simulator: a live
clock/date (`Clock` in `src/core/`, publishing once a second — and once
immediately at startup, so the display never shows a placeholder before
the first tick — through the `EventBus`) and a battery percentage
(`BatteryReader`, a fixed mock value for now — see
[simulator.md](simulator.md#how-it-works) for why). Both are currently
hardcoded directly on `DashboardScreen`, standing in for what M2 turns
into shared status-bar chrome present on every screen (see [Status
bar](#status-bar) above) — not dashboard-grid widgets. No pluggable
widget-registration system, grid layout, or status bar exists yet; those
stay M2. See `docs/roadmap.md` for what's next.
