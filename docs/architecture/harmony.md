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
`hub_host` is validated client-side before saving (rejecting a scheme
prefix, embedded whitespace, or a path, all of which would otherwise
produce the same opaque connection failure as an unreachable address;
see `webui/src/lib/harmonyValidation.ts`).

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
cycle otherwise (`kLivenessInterval`, 30s by default) — freshness is
best-effort, not push-driven, since this class's WebSocket transport is
a simple synchronous request/response loop; see that class's own header
comment for why. `ActivitiesScreen` (`src/ui/screens/activities_screen.h`/
`.cpp`) is the Touch UI's primary Harmony surface, reached by tapping
`HarmonyWidget` (`src/ui/harmony_widget.h`/`.cpp`) on the dashboard —
the current activity, not an app-launcher grid, per
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md)'s dashboard
philosophy.

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

## Web UI

`HarmonySettings.svelte` (`webui/src/lib/`) follows
`WeatherSettings.svelte`'s established pattern: reads/writes `hub_host`
through the generic settings API, shows live connection status via
`GET /api/harmony/status` with a manual Refresh button (no live-push
mechanism exists for the Web UI yet — see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server)),
and triggers `POST /api/harmony/reconnect` on save.

## Status

Implemented and complete against this document's own scope — connection,
activities, devices, remote control (including long-press), status/event
integration, and the Web UI settings page. First-time cloud pairing is
out of scope, unconfirmed either way — this module only ever connects
to an already-paired hub, per
[ADR-0003](../decisions/ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control).
