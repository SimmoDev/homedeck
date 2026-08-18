# Harmony Module

Harmony is HomeDeck's reference module (see
[modules.md](modules.md) for the general module contract and
[ADR-0003](../decisions/ADR-0003-module-architecture.md) for the
architectural reasoning) and the first supported integration named in
[CLAUDE.md](../../CLAUDE.md). It replaces a physical Logitech Harmony
Hub remote's day-to-day usage — starting activities, sending device
commands — for an already-paired hub on the same LAN.

## Connection

`HarmonyConnection` (`src/core/harmony_connection.h`/`.cpp`) is a
`Module` (see [modules.md#status](modules.md#status)) that owns a
background `Task` running a connect/retry loop. The hub address is a
single manually-entered setting (module `harmony`, key `hub_host`),
stored and read through the generic settings API
(`src/core/settings_routes.h`) rather than a Harmony-specific endpoint —
`hub_host` is validated both client-side (`webui/src/lib/harmonyValidation.ts`)
and server-side (`IsValidHubHost()`, `src/core/harmony_connection.h`,
wired into `RegisterSettingsRoutes()`'s generic `SettingValidateFn` from
`src/ui/app_core.cpp`) — `SettingValidateFn` runs on both the direct
`POST /api/settings` write and `POST /api/backup/restore`'s replay, so a
malformed `hub_host` restored from a backup file is rejected the same
way, reported back in that response's `rejected` array rather than
silently persisted. A scheme prefix, embedded whitespace, or a path
would otherwise produce the same opaque connection failure as an
unreachable address; `#`, `?`, and `@` are rejected for a different,
more serious reason — each one changes what the underlying URL parser
treats as the actual host/port, silently connecting to a different
destination rather than just failing to connect at all.

There is no discovery protocol and no authentication step in this
protocol — see [ADR-0029](../decisions/ADR-0029-harmony-local-protocol.md)
for the full protocol facts (local WebSocket/JSON API on port 8088, a
plain HTTP handshake with no credential) and why a discovery prober
wasn't built. `GET /api/harmony/status` and
`POST /api/harmony/reconnect` (`src/core/harmony_routes.h`/`.cpp`,
admin-gated) expose connection state and an immediate-reconnect trigger
to the Web UI.

Connection failures use the shared `RetryBackoff` utility
([ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-retrybackoff-policy-ownership)).
`HarmonyConnectionState` (`kDisconnected`/`kConnecting`/`kConnected`/
`kError`) is published on every transition
(`HarmonyConnectionStateChangedEvent`) over the `EventBus`.

## Activities

`HarmonyConnection` fetches the hub's activity list on connect and
tracks `current_activity_id`, refreshed on connect, right after
`StartActivity()` sends its command, and on the periodic liveness-probe
cycle otherwise (`liveness_interval`, 30s by default) — freshness is
best-effort, not push-driven, since this class's WebSocket transport is
a simple synchronous request/response loop; see that class's own header
comment for why. `ActivitiesScreen` (`src/ui/screens/activities_screen.h`/
`.cpp`) is the Touch UI's primary Harmony surface, reached by tapping
`HarmonyWidget` (`src/ui/harmony_widget.h`/`.cpp`) on the dashboard —
the current activity, not an app-launcher grid, per
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md)'s dashboard
philosophy.

Tapping the activity that's already running resends its own start
command instead of treating the tap as a no-op — useful to resync AV
gear that's drifted out of sync with the hub's own idea of what's on,
matching the official Harmony app/remote's own re-trigger support.
`ActivitiesScreen` shows "Resent to `<name>`." for this case, distinct
from "Starting `<name>`..." for a fresh activity switch.

Activities render in the order the hub's config response lists them
(`ParseIdLabelArray()`, `src/core/harmony_connection.cpp`) — neither this
project's own live-hub probe (see ADR-0029) nor `aioharmony` (the
actively-maintained community client library ADR-0029 cites) found a
distinct sequence/order field in the config payload, so array order is
the only ordering this protocol exposes. The official Harmony app's own
"Reorder Activities" feature isn't backed by one shared hub-side order
either — Logitech's own documentation states reordering in the app and
reordering on the physical remote don't sync to each other — so there is
no known "official order" for HomeDeck to mirror even in principle.
Devices below follow the same array-order rule.

## Devices and remote control

A device's capabilities and its remote-control commands are the same
data (`HarmonyDevice::control_groups`, each a named
`HarmonyControlGroup` of `HarmonyCommand`s) — there's no separate
"capabilities" structure to browse before commands become sendable.
"Power state" (a device's `Capabilities`/`powerFeatures` fields) isn't
surfaced: empty on every device this project has tested against, since
IR is one-way with nothing to poll.

`DevicesScreen` (`src/ui/screens/devices_screen.h`/`.cpp`) is reached
from `ActivitiesScreen`'s "Devices" button — one screen with two
internal view states (device list, then a selected device's commands)
rather than a second `Navigation` route, since this project's
`Navigation` has no back-stack. Commands render as a 3-per-row grid by
default; two groups recognized by the hub's own protocol-level
`HarmonyControlGroup::name` vocabulary get a dedicated layout instead —
`NumericBasic` as a 1-2-3/4-5-6/7-8-9/Clear-0-Dot keypad, `NavigationBasic`
as a D-pad cross — matched by name across every device, not
per-device-model hardcoding.

Sending a command is three separate calls —
`HarmonyConnection::PressDeviceCommand()`/`HoldDeviceCommand()`/
`ReleaseDeviceCommand()` — mirroring the hub's own three-state
`holdAction` protocol, driven by `DevicesScreen`'s
`LV_EVENT_LONG_PRESSED`/`LONG_PRESSED_REPEAT`/`RELEASED` handlers so a
sustained hold repeats and a drag that starts on a command button sends
nothing. All three calls are safe from any thread and queue onto
`HarmonyConnection`'s own connection-loop thread
(`pending_commands_`, capped at 20 entries and dropping any entry older
than 5s by default — both bound how much a burst of taps/holds during a
brief connectivity drop can replay, stale, all at once on reconnect).
A send failure has no result value to check
(`Press`/`Hold`/`ReleaseDeviceCommand()` are all `void`); the only
in-screen signal is `DevicesScreen`'s own status label reacting to
`HarmonyConnectionStateChangedEvent`'s `kError` state.

## Status and notifications

`HarmonyConfigUpdatedEvent` and `HarmonyCurrentActivityChangedEvent`
join `HarmonyConnectionStateChangedEvent` on the `EventBus`.
`HarmonyNotificationBridge` (`src/core/harmony_notification_bridge.h`/
`.cpp`) subscribes to connection-state changes directly and publishes a
`NotificationEvent` once on entering `kError` — latched the same way
`LowBatteryMonitor`'s own notification is, so a sustained outage's
retry/backoff loop doesn't publish one notification per attempt.

A fourth event, `HarmonyCommandDroppedEvent`, is published when
`SendPendingCommands()` drops a queued command, either for being older
than `max_pending_command_age_` (a connection outage outlasted the
pending-command queue's own staleness bound, so the command never
reached the hub and never will) or because a send partway through the
batch failed, taking every remaining queued command in that batch with
it. `ActivitiesScreen`'s own `dropped_sub_` is the only subscriber,
reporting "Couldn't start `<name>` - hub unreachable" for the cases none
of the other three events cover: a tap that ages out before a connection
ever comes back to send it, or one lost to a mid-batch send failure.

## Web UI

`HarmonySettings.svelte` (`webui/src/lib/`) follows
`WeatherSettings.svelte`'s established pattern: reads/writes `hub_host`
through the generic settings API, shows live connection status via
`GET /api/harmony/status` with a manual Refresh button (no live-push
mechanism exists for the Web UI yet — see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server)),
and triggers `POST /api/harmony/reconnect` on save. Saving with the
field empty un-configures the hub — `IsValidHubHost()` accepts empty
(see its own comment) and `ConnectionLoop()` treats it as "not yet
configured," so the reconnect trigger disconnects any active connection
the same way a first-time save connects one. Unlike a transient
disconnect/error (which keeps showing the last-known devices/activities —
see `HarmonyConnectionSnapshot::has_config`'s own comment), un-configuring
or pointing `hub_host` at a *different* address both clear them
(`HarmonyConnection::ClearConfigIfPresent()`, compared against the
previously-attempted address on every `ConnectionLoop()` pass) and
publish `HarmonyConfigUpdatedEvent`, so `ActivitiesScreen`/
`DevicesScreen`/the Web UI's own status panel drop the previous hub's
now-meaningless list instead of continuing to render it as if still
live.

## Status

Implemented and complete against this document's own scope — connection,
activities, devices, remote control (including long-press), status/event
integration, and the Web UI settings page. First-time cloud pairing is
out of scope, unconfirmed either way — this module only ever connects
to an already-paired hub, per
[ADR-0003](../decisions/ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control).
