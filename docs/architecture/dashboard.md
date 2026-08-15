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
despite being listed as example widgets in [CLAUDE.md](../../CLAUDE.md) — see [Status
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
  geocoding API - see [Status](#status) below). Implemented as of M2,
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

This follows directly from the offline-behavior requirements in [CLAUDE.md](../../CLAUDE.md)
(see also [networking.md](networking.md#offline-behaviour)) — a widget
showing stale Home Assistant state during a Wi-Fi outage must not look
identical to one showing current state.

## Customization (future)

[CLAUDE.md](../../CLAUDE.md) establishes an eventual goal of user-customizable:

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

The dashboard shell runs on both targets — the simulator and real Tab5
hardware (see [hardware.md](hardware.md#on-device-dashboard)).
`StatusBar` (`src/ui/status_bar.h`/`.cpp`) is fixed, non-scrolling
chrome, constructed by every screen (`DashboardScreen` and
`WifiSetupScreen`, proving the "every screen" mechanism — see
[ui.md](ui.md#status)): a solid black bar with white text showing
compact date/time (subscribed to `Clock`'s `ClockTickEvent`, blank
until the first tick rather than a fabricated time) and battery
percentage, both refreshed on that same tick rather than a second
timer, per
[ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets)'s
"relocate, don't rebuild" framing. Black/white styling matches the
Android/iOS convention that ADR references.

Font size is Montserrat 24: on the panel's ~294 PPI (from its 5"
720×1280 spec, see [hardware.md](hardware.md#display-and-touch)), that's
the closest match among LVGL's available discrete sizes to Android's
typical status bar text by physical glyph height (~2.1mm vs. ~2.2mm) —
enabled identically on both targets (see `simulator/lv_conf.h` and
`firmware/sdkconfig.defaults`).

`BatteryReader` is mocked in the simulator (a fixed value, adjustable via
debug buttons — see [simulator.md](simulator.md#how-it-works)) but real on
firmware, reading the INA226 power monitor. The status bar reflects three
states (see [hardware.md](hardware.md#power) for how each is derived):
battery percentage alone; a charge icon plus percentage while a present
battery is actually charging; and a USB icon with no percentage when no
battery is installed, since the percentage reading isn't meaningful in
that state. `ReadPercent()`'s own approximation accuracy is unchanged
by this — see [hardware.md](hardware.md#power) for its known margin; a
real state-of-charge estimate is still Power Management scope, not the
status bar's.

The status bar's Wi-Fi connectivity icon works the same way:
`StatusBar` prepends `LV_SYMBOL_WIFI` to the same label as the battery
icon/percentage (present or absent, not glyph-swapped) rather than a
separately-positioned object, so LVGL's own text layout keeps the
spacing between every symbol uniform. The tracked connected state
updates on the portable `WifiConnectivityChangedEvent` (see
[networking.md](networking.md#status)) rather than the 1Hz clock tick,
since connectivity is a discrete, rare transition rather than a
slowly-drifting value - the label itself still only repaints on the
clock tick or on that event, whichever comes first. The fuller
network-status grid widget this doc's [Widget
system](#widget-system) section names above remains a separate,
not-yet-built follow-up.

The widget framework's interface and layout half is implemented: `Widget`
(`src/ui/widget.h`) is the standard contribution interface — a `Root()`
accessor, a `ColumnSpan()`/`RowSpan()` footprint (both default to 1,
override to occupy more), and an `OnTap()` handler (no-op default;
`DashboardGrid::AddWidget()` wires every widget's `Root()` to it
uniformly) - deferred until a widget genuinely needed tap-for-detail
(`HarmonyWidget` below, M3), not designed speculatively against an
earlier widget alone. No live/cached/offline freshness reporting yet,
left out as ADR-0008 says it should be until a real widget exists to
design against. `DashboardGrid` (`src/ui/dashboard_grid.h`/`.cpp`)
hosts widgets on a fixed, first-fit-packed grid — see its own header
comment for the column count, row-growth, and placement-algorithm
rationale. Built into `DashboardScreen` on both targets.

`ClockWidget` (`src/ui/clock_widget.h`/`.cpp`) is the first grid widget
built on that placement mechanism: a large time/date display, the
"optional larger clock widget" this doc's own [Widget
system](#widget-system) section named, distinct from the always-on
status bar clock. Portable and target-agnostic, the same
`EventBus::SubscribeUi<ClockTickEvent>` mechanism `StatusBar` already
uses, just a bigger font (Montserrat 48, enabled identically on both
targets alongside the status bar's own Montserrat 24) and a 2-column
span.

`NetworkStatusWidget` (`src/ui/network_status_widget.h`/`.cpp`) is the
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
leaving dead space below it. Known gap: `NetworkStatusMonitor` only
publishes on a connected/disconnected transition, so an IP change that
doesn't pass through a disconnect (e.g. a DHCP lease renewal while
still associated) won't refresh this widget's IP label - not observed
in practice and not worth a second event/poll path until it is.

`WeatherWidget` (`src/ui/weather_widget.h`/`.cpp`) is backed by
`OpenMeteoWeatherProvider` (`src/core/weather_provider.h`/`.cpp` - see
[Weather source](#weather-source) below and
[networking.md](networking.md#status) for the outbound `HttpClient`
interface it's built on). A 2-column tile showing temperature and a WMO
condition-code text mapping (no custom icons yet - see the roadmap's M7
polish item), plus the configured location's display name. Renders one
of three states: not configured ("Set a location in Settings"), a live
reading, or a cached/stale reading marked as such - the first widget to
exercise this doc's own [Data freshness](#data-freshness) requirement,
per ADR-0008's note that it was left undesigned until a real widget
existed to design against. Polls Open-Meteo every 30 minutes on a
dedicated background `Task` (`src/platform/task.h`), not `Timer` -
FreeRTOS's software timers share one timer-service task sized for
lightweight callbacks (`Clock`'s own tick), and a multi-second HTTPS
fetch would stall every other timer system-wide if run there. A
condition variable, not a plain sleep, governs the wait between polls -
`OpenMeteoWeatherProvider::TriggerPoll()` notifies it to wake the loop
immediately, so choosing a new location doesn't leave the dashboard
waiting out the rest of a real 30-minute interval in silence; the
Web UI's Settings page calls this (via `POST /api/weather/refresh`, see
[web-ui.md](web-ui.md#status)) right after saving a newly-selected
location. Location search goes through Open-Meteo's own geocoding API,
proxied through an admin-gated `GET /api/weather/geocode` endpoint.
`Storage` (`src/core/storage.h`) is internally thread-safe (a single
mutex guarding every method) - genuinely needed, not defensive:
app_main's boot sequence, the Web UI's httpd worker thread, and this
widget's poll `Task` all call into the same `Storage` instance with no
coordination between them.

`NotificationWidget` (`src/ui/notification_widget.h`/`.cpp`) is the
dashboard-indicator output [CLAUDE.md](../../CLAUDE.md)'s notification
requirements name (see [ui.md](ui.md#notification-presentation)) - a
2-column tile always echoing the most recent `NotificationEvent`'s
message ("No notifications yet" until the first one arrives), not an
unread-count badge - see [roadmap.md](../roadmap.md)'s M7 list for that
variant, deliberately deferred since nothing today fires more than one
notification per episode. Subscribes directly to `NotificationEvent`,
the same as `NotificationBanner`'s own screen-banner presentation of
the same event - no new Core-owned state exists purely to back this
widget.

`HarmonyWidget` (`src/ui/harmony_widget.h`/`.cpp`) is the "Current
Harmony activity" tile this doc's [Widget system](#widget-system)
section names above, and the framework's first `OnTap()` implementation
— tapping it opens `ActivitiesScreen` (`src/ui/screens/activities_screen.h`/
`.cpp`, see [roadmap.md](../roadmap.md)'s M3 Activities item), not a
launcher grid ([ADR-0004](../decisions/ADR-0004-ui-philosophy.md)). A
2-column tile showing the connection state, or (once connected) the
current activity's own label, sourced from `HarmonyConnection`
(`src/core/harmony_connection.h`).
