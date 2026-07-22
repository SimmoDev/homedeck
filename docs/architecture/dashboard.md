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
  choosing a location via a place-name search (also Open-Meteo, its free
  geocoding API - see [Status](#status) below). Real as of M2,
  independent of any module.
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

The status bar's Wi-Fi connectivity icon is also real, confirmed on
hardware: `StatusBar` prepends `LV_SYMBOL_WIFI` to the same label as the
battery icon/percentage (present or absent, not glyph-swapped) rather
than a separately-positioned object, so LVGL's own text layout keeps the
spacing between every symbol uniform. The tracked connected state
updates on the portable `WifiConnectivityChangedEvent` (see
[networking.md](networking.md#status)) rather than the 1Hz clock tick,
since connectivity is a discrete, rare transition rather than a
slowly-drifting value - the label itself still only repaints on the
clock tick or on that event, whichever comes first. The fuller
network-status grid widget this doc's [Widget
system](#widget-system) section names above remains a separate,
not-yet-built follow-up.

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
source](#weather-source) above) is also real - see below.

**The network status widget is also real**: `NetworkStatusWidget`
(`src/ui/network_status_widget.h`/`.cpp`) is the
"fuller network status grid widget for detail beyond what an icon can
show" this doc's [Status bar](#status-bar) section names above - a
2-column tile showing the Wi-Fi icon plus connected/disconnected state,
and (once connected) SSID and IP, sourced from the same
`NetworkStatus`/`WifiConnectivityChangedEvent` the status bar's icon
uses (see [networking.md](networking.md#status)). SSID is truncated
with an ellipsis at its real 32-character maximum rather than wrapped,
since a wrapped second line would push the IP label out of the tile.
When disconnected, the SSID/IP labels are hidden rather than blanked,
so a lone "Disconnected" line stays centered in the tile instead of
leaving dead space below it. **Confirmed in the simulator**: renders
correctly alongside `ClockWidget`, both connected (real SSID/IP) and
disconnected (exercised via a simulator debug button) states.
**Confirmed on the Tab5 K145 reference unit**, including the rendered
layout on the real panel: boots without a crash, the real Wi-Fi SSID/IP
reach it (`Dashboard loaded` followed by a clean heartbeat, confirmed
via serial log), and it renders correctly alongside `ClockWidget`. Known
gap: `NetworkStatusMonitor` only publishes on a
connected/disconnected transition, so an IP change that doesn't pass
through a disconnect (e.g. a DHCP lease renewal while still associated)
won't refresh this widget's IP label - not observed in practice and not
worth a second event/poll path until it is.

**The weather widget is also real**: `WeatherWidget`
(`src/ui/weather_widget.h`/`.cpp`), backed by `OpenMeteoWeatherProvider`
(`src/core/weather_provider.h`/`.cpp` - see [Weather
source](#weather-source) below and
[networking.md](networking.md#status) for the new outbound `HttpClient`
interface it's built on). A 2-column tile showing temperature and a WMO
condition-code text mapping (no custom icons yet - see the roadmap's M7
polish item), plus the configured location's display name. Renders one
of three states: not configured ("Set a location in Settings"), a live
reading, or a cached/stale reading marked as such - the first widget to
exercise this doc's own [Data freshness](#data-freshness) requirement
for real, per ADR-0008's note that it was left undesigned until a real
widget existed to design against. Polls Open-Meteo every 30 minutes on
a dedicated background `Task` (`src/platform/task.h`), not `Timer` -
FreeRTOS's software timers share one timer-service task sized for
lightweight callbacks (`Clock`'s own tick), and a multi-second HTTPS
fetch would stall every other timer system-wide if run there. A
condition variable, not a plain sleep, governs the wait between polls -
`OpenMeteoWeatherProvider::TriggerPoll()` notifies it to wake the loop
immediately, so choosing a new location doesn't leave the dashboard
waiting out the rest of a real 30-minute interval in silence; the
Web UI's Settings page calls this (via `POST /api/weather/refresh`, see
[web-ui.md](web-ui.md#status)) right after saving a newly-selected
location. Confirmed end to end against the simulator, a real browser
session, and the Tab5 K145 reference unit (location search via
Open-Meteo's own geocoding API, proxied through a new admin-gated
`GET /api/weather/geocode` endpoint, selecting a result, and
`TriggerPoll()` firing a real fetch immediately - a real TLS handshake
visible in the reference unit's own serial log right after). `Storage`
(`src/core/storage.h`) is internally
thread-safe (a single mutex guarding every method) - genuinely needed,
not defensive: app_main's boot sequence, the Web UI's httpd worker
thread, and this widget's poll `Task` all call into the same `Storage`
instance with no coordination between them.
