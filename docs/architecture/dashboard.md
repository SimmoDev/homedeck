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
affordance](ui.md#navigation-model). Network status lives in both places,
not one or the other: a compact Wi-Fi connectivity icon here (see
[roadmap.md](../roadmap.md)'s Status bar item), alongside a fuller
network status grid widget for detail beyond what an icon can show (see
[Widget system](#widget-system) above) — the same compact/detailed split
already established for the clock. See
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

The initial dashboard shell exists and runs for real on both targets: the
simulator, and real Tab5 hardware (see
[hardware.md](hardware.md#on-device-dashboard)). The status bar is real,
confirmed on hardware — `StatusBar` (`src/ui/status_bar.h`/`.cpp`) is
fixed, non-scrolling chrome, constructed by every screen
(`DashboardScreen` and, proving the "every screen" mechanism on both
targets, `WifiSetupScreen` — see [ui.md](ui.md#status)), rendered as a
solid black bar with
white text: compact date/time (subscribed to `Clock`'s existing
`ClockTickEvent`, publishing once a second; blank until the first tick
arrives rather than showing a fabricated time) and battery percentage,
both refreshed on that same tick rather than a second timer, per
[ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets)'s
"relocate, don't rebuild" framing. Black/white styling matches the
Android/iOS convention that ADR already referenced.

Font size is Montserrat 24: on the panel's ~294 PPI (from its 5"
720×1280 spec, see [hardware.md](hardware.md#display-and-touch)), that's
the closest match among LVGL's available discrete sizes to Android's
typical status bar text by physical glyph height (~2.1mm vs. ~2.2mm) —
enabled identically on both targets (see `simulator/lv_conf.h` and
`firmware/sdkconfig.defaults`).

`BatteryReader` is mocked in the simulator (a fixed value, adjustable via
debug buttons — see [simulator.md](simulator.md#how-it-works)) but real on
firmware, reading the INA226 power monitor. The status bar reflects three
real states, confirmed on hardware (see [hardware.md](hardware.md#power)
for how each is derived): battery percentage alone; a charge icon plus
percentage while a present battery is actually charging; and a USB icon
with no percentage when no battery is installed, since the percentage
reading isn't meaningful in that state. `ReadPercent()`'s own approximation
accuracy (confirmed slightly inaccurate, e.g. reading ~90% on a pack
already fully charged) is unchanged by this — a real state-of-charge
estimate is still Power Management scope, not the status bar's.

The widget framework's interface and layout half is real: `Widget`
(`src/ui/widget.h`) is the standard contribution interface — a `Root()`
accessor plus a `ColumnSpan()`/`RowSpan()` footprint (both default to 1,
override to occupy more); no live/cached/offline freshness reporting
yet, left out as ADR-0008 says it should be until a real widget exists
to design against. `DashboardGrid` (`src/ui/dashboard_grid.h`/`.cpp`)
hosts widgets on a fixed grid — `DashboardGrid::kColumns` (4) columns,
genuinely arbitrary pending real content to size against, not a
considered choice — with rows growing on demand as widgets are added (no
paging concept exists, so the grid stays scrollable for when content
exceeds the visible screen, unlike the deliberately non-scrolling status
bar). Rows are a fixed height matched to column width, so a 1×1 cell is
square rather than sized to its own content. Placement is first-fit:
scan cells top-to-bottom, left-to-right, and use the first position a
widget's full footprint fits without overlapping an already-placed
widget, tracked via a real per-row occupancy bitset, not just a simple
left-to-right cursor (multi-row spans need to know which cells further
down are already taken), including the 8px gap between cells (folded
into the same row-height calculation, not applied separately, since it
eats into the same available width the column size comes from). Built
into `DashboardScreen` on both targets, confirmed on hardware (Tab5 K145
reference unit): first-fit placement of mixed spans, square 1×1 cells,
and cell spacing all render correctly on the real panel.

**The first real grid widget is built**: `ClockWidget`
(`src/ui/clock_widget.h`/`.cpp`) - a large time/date display, the
"optional larger clock widget" this doc's own [Widget
system](#widget-system) section named, distinct from the always-on
status bar clock. Portable and target-agnostic, the same
`EventBus::SubscribeUi<ClockTickEvent>` mechanism `StatusBar` already
uses, just a bigger font (Montserrat 48, enabled identically on both
targets alongside the status bar's own Montserrat 24) and a 2-column
span. It replaces the four throwaway widgets (`PlaceholderWidget` in
`simulator/widgets/`, an identical inline `TestWidget` in
`firmware/main/homedeck.cpp`) that previously proved `DashboardGrid`'s
mixed-span placement mechanism — both removed now that a real widget
exists, per this section's own original intent. **Confirmed on
hardware** (Tab5 K145 reference unit): renders centered and legible,
with no clipping or overlap. Weather (see [Weather
source](#weather-source) above) remains a separate follow-up pass.
