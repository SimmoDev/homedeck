# Kodi Module

Kodi is HomeDeck's second integration (see [modules.md](modules.md) for
the module contract and [ADR-0003](../decisions/ADR-0003-module-architecture.md)
for the reasoning) and the first Media module named in
[CLAUDE.md](../../CLAUDE.md). It gives HomeDeck a Now Playing view, a
transport remote, and a UI-navigation D-pad for a Kodi instance on the
same LAN.

The transport decision and its verified protocol facts are in
[ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md). This
document describes how the module works.

## Connection

`KodiClient` (`src/core/kodi_client.h`/`.cpp`) is a `Module` (see
[modules.md#status](modules.md#status)) that owns a background `Task`
running a resolve/connect/reconcile loop. It connects to Kodi's JSON-RPC
API over an **unauthenticated WebSocket on port 9090**, path `/jsonrpc`
([ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md)) - reusing
the `WebSocketClient` platform capability built for Harmony in M3. There
is no credential of any kind.

Connection failures use the shared `RetryBackoff` utility
([ADR-0006](../decisions/ADR-0006-networking-discovery-provisioning.md#decision-retrybackoff-policy-ownership)).
`KodiConnectionState` (`kDisconnected`/`kConnecting`/`kConnected`/
`kError`) is published on every transition
(`KodiConnectionStateChangedEvent`) over the `EventBus`.

### "Not reachable" is a normal state

A failed or dropped connection publishes **no `NotificationEvent`** -
there is no `HarmonyNotificationBridge` equivalent. On Android/Google
TV, Kodi is an app that is frequently not running, and its remote-
control API disappears with it (both port 9090 and 8080 stop answering
within minutes of the app being backgrounded); "unreachable" is the
resting state for a large share of setups, not a fault. The
dashboard widget and screens show a plain "not reachable" indicator and
the retry loop continues quietly.

## Discovery and instance selection

Core's mDNS browsing wrapper (`src/platform/mdns_browser.h`, see
[networking.md](networking.md#status)) browses `_xbmc-jsonrpc._tcp`.
Kodi is its first consumer.

`ResolveTarget()` picks what to connect to on each loop pass:

- A manually-entered **`host` override** (module `kodi`, key `host`,
  validated by `IsValidKodiHost()` - same shape as `IsValidHubHost()`)
  wins and skips discovery entirely.
- Otherwise the browse result is matched against the saved
  **`instance_uuid`** (module `kodi`, key `instance_uuid`) - the `uuid`
  from the instance's mDNS TXT record, **not its IP**, so a DHCP lease
  change or a Kodi restart doesn't lose the selection.
- If nothing is saved and exactly one instance answered, it is
  auto-selected. If more than one answered and nothing is saved, the
  module stays `kDisconnected` and the UI asks the user to choose - it
  never guesses.
- A saved-but-offline instance does **not** fall back to another
  discovered instance; silently controlling a different room's Kodi
  would be worse than doing nothing.

The `host` and `instance_uuid` keys are mutually exclusive; the Web UI
writes one and clears the other.

## Now Playing

Now Playing state is **event-driven**, not polled - the main reason for
the port-9090 transport. Kodi pushes JSON-RPC notifications
(`Player.OnPlay` / `OnAVStart` / `OnAVChange` / `OnPause` / `OnResume` /
`OnSpeedChanged` / `OnStop`, `Application.OnVolumeChanged`) unsolicited
to every connected client, with no subscribe call.

`KodiClient`'s connection loop:

- **Drains pushed notifications** every `pump_interval` (250 ms) via a
  non-blocking `ReceiveText(0)`.
- **Correlates responses to its own requests by numeric `id`**
  (`Call()`), dispatching any notification frames that arrive
  interleaved while it waits for a reply. This is the step up from
  Harmony's "send one, read one" loop.
- Runs a **reconcile poll** on connect, every `reconcile_interval`
  (10 s), and immediately after any play-state notification (those
  carry no timing fields). The poll is `Application.GetProperties` →
  `Player.GetActivePlayers` → `Player.GetProperties` + `Player.GetItem`,
  and doubles as the liveness check - a transport failure during it
  triggers a reconnect.

### Identity vs. timing

Per [ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md), the
notification's own `item` is the authoritative source of what's playing
(title / show / season / episode). For add-on playback (e.g. a
`plugin.video.*` source) `Player.GetItem` returns `type:"unknown"` with
blank fields, while the notification `item` still has the populated values.
`KodiClient` therefore uses `Player.GetItem`'s identity only as the
*initial* value when it connects to a Kodi that is already playing (no
notification seen yet); once any `Player.On*` notification supplies an
identity, the poll stops overwriting it. Progress / duration / speed
always come from `Player.GetProperties`.

### Progress freshness

Kodi pushes nothing as playback simply advances - a notification fires
only on a state change (play / pause / seek / speed). So during
uninterrupted playback the position, the `NowPlayingScreen` time label,
and its `lv_bar` only move on the `reconcile_interval` poll: they step
forward every 10 s rather than ticking smoothly, then re-sync
immediately after any transport action (a seek notification sets
`needs_immediate_poll_`). This is a deliberate tradeoff against a
local interpolation timer, not a bug; a smoother bar is left to M7
polish.

### Notes on the reference build (Kodi 21 "Omega", Android)

- Every `Player.On*` notification's `params.data.player.playerid` is
  `-1`. `KodiClient` never trusts it - it resolves the true id via
  `Player.GetActivePlayers` before any player-scoped call.
- `Player.OnStop` has no `player` object; `params.data.end`
  distinguishes "played to completion" from "stopped by the user".
- `Application.OnVolumeChanged` fires for JSON-RPC volume changes but
  **not** for the TV's own hardware volume keys (those are Android
  system volume, outside Kodi) - so volume is also read on the
  reconcile poll, not treated as push-only.
- `Player.GetItem` rejects `*id` request fields (`episodeid`, …) with
  `-32602`; Kodi returns `id` + `type` automatically.

## Commands

`PlayPause()` / `StopPlayback()` / `SeekPercent()` / `SetSpeed()` /
`SetVolume()` / `ToggleMute()` / `SendInput()` / `OpenLibraryItem()` are
safe to call from any thread. They queue the intent onto the connection
loop, which owns the socket, and are **fire-and-forget** - Kodi's own
pushed notification, not the reply, is what refreshes the snapshot; the
reply frame is discarded by the notification pump.

The queue is bounded (drop-oldest when full) and drops entries older
than `max_pending_command_age`, mirroring
`HarmonyConnection::pending_commands_`. `StopPlayback()` and mute are
exempt from the staleness drop - like Harmony's `release`, they settle
something already happening on the box, so an aged-out one is still sent
on the next drain. A command is still lost if the transport fails while
its own batch is draining (the batch is not requeued); because Now
Playing is push-driven, the next reconcile poll re-syncs state, so a
lost command leaves nothing stuck.

Player-scoped commands (`PlayPause` etc.) can't be built until the loop
thread resolves the active `playerid`; `SendPendingCommands()` resolves
it once per batch via `Player.GetActivePlayers`. `SendInput()` uses the
dedicated `Input.Up`/`Down`/…/`ShowOSD` methods, not
`Input.ExecuteAction` (its action-name validation was inconsistent on
the reference build - [ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md)).

## Touch UI

`KodiWidget` (`src/ui/kodi_widget.h`/`.cpp`) is the dashboard's second
module tile, after `HarmonyWidget`. It shows what Kodi is playing / the
connection state and, on tap, opens `NowPlayingScreen`.

`NowPlayingScreen` (`src/ui/screens/now_playing_screen.h`/`.cpp`) - built
on the shared `ScreenChrome` (`src/ui/screens/screen_chrome.h`, factored
out of Harmony's screens) - has a subtitle, an `lv_bar` progress bar
with an `m:ss` time label, and transport buttons (seek ±, play/pause,
stop, volume ±, mute) wired to the `KodiClient` commands. The seek
buttons disable themselves (`LV_STATE_DISABLED`) when
`KodiNowPlaying::can_seek` is false - `Player.GetProperties`'s own
`canseek` property, false for some live/add-on sources Kodi can play
but not scrub through, where a seek command would otherwise be a
silent no-op. It is fully push-driven: every element re-renders from
`KodiClient::Snapshot()` on each Kodi event, with **no optimistic
local state** (unlike
`ActivitiesScreen`, whose optimistic-status machinery exists only
because Harmony's IR path gives no confirmation). A "Remote" button
opens:

`KodiRemoteScreen` (`src/ui/screens/kodi_remote_screen.h`/`.cpp`) - a
D-pad plus Back / Home / Info / OSD / Menu, each a plain-tap
`SendInput()`. It builds its own small cross rather than sharing
`DevicesScreen`'s D-pad: Harmony's D-pad cells are press/hold/release
command buttons, Kodi's are a single tap.

The display-string formatting (widget line, Now Playing subtitle,
`m:ss` clock) is `src/ui/kodi_display.h`/`.cpp` - LVGL-free and
host-tested, the same split `ui/text_format.h` uses.

## Web UI

`KodiSettings.svelte` (`webui/src/lib/`) reads/writes `host` /
`instance_uuid` through the generic `/api/settings` API, shows a radio
list of discovered instances (or a manual-address field), and shows
live status via `GET /api/kodi/status` with a manual Refresh button (no
live-push mechanism exists for the Web UI yet). Saving triggers
`POST /api/kodi/reconnect` (`src/core/kodi_routes.h`/`.cpp`).

## Status

Implemented for M4a: discovery/selection, connection, Now Playing state
and the transport/nav Touch UI, the fire-and-forget command surface,
the two Web UI routes and the settings page.

`KodiClient`'s connect/reconcile/notification loop and its
discovery/selection policy are host-tested against fake `MdnsBrowser` /
`WebSocketClient` doubles plus one test over a libcurl-backed
`HostWebSocketClient` and a raw-socket loopback JSON-RPC peer
(`tests/kodi_client_test.cpp`). The `MdnsBrowser` backend adapters
themselves are not unit-tested and the firmware one is on-device only —
see [networking.md](networking.md#status) for why, and for which part of
M4 that verification belongs to.

`NowPlayingScreen` / `KodiRemoteScreen`'s populated content (the
transport row with `SetTransportGlyph()`'s double-triangle rewind/
fast-forward glyph, the volume row, the D-pad) lives inside `content_`,
which stays hidden until `KodiClient::Snapshot().state == kConnected`
(see `NowPlayingScreen::Refresh()`). In the simulator that state needs
either a Kodi instance on the LAN or the "Test: toggle fake Kodi
connection" debug control (see
[simulator.md](simulator.md#status), `simulator/debug_kodi_backend.cpp`).

**Not yet built (M4b):** library browsing (movies / TV / music / files /
live TV / recently added / continue watching) and the screens for it.
`OpenLibraryItem()` is the plumbing already in place for it. Artwork is
out of scope until M7 - the `image://…` URLs Kodi returns resolve only
through its HTTP endpoint on the authenticated port 8080
([ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md)).
