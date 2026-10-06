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
  wins and skips discovery entirely. The override always connects on port
  9090 - `IsValidKodiHost()` rejects `:`, so a non-default port can't be
  entered this way; a Kodi on a custom RPC port has to be reached through
  discovery.
- A discovered instance's host never passes through `IsValidKodiHost()`
  (it didn't come from the user), so `ResolveTarget()` drops any whose
  host carries userinfo, a path, or whitespace/control bytes before it
  can reach the `ws://` URL - a hostile mDNS responder on the
  unauthenticated LAN otherwise redirects the connection.
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
- An instance with no resolved IP address - only `MdnsService`'s own
  `hostname` fallback field - is never auto-selected or matched against
  a saved `instance_uuid`, for either of the above. `hostname` is a
  best-effort display value only (its own header warns `.local`
  resolution isn't guaranteed on either target); a device can
  advertise a hostname with no domain suffix at all ("Android", not
  "Android.local"), which can never resolve through a generic connect.
  Still shown in the Web UI's discovered-instance list so the user can
  enter its address manually - just never connected to automatically.

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

Identity fields are merged, never individually cleared, so a
`Player.OnPlay` for a new item - a playlist advancing, or a movie
started without an intervening `Player.OnStop` - first resets
title / show / season / episode / type, then applies the new
notification's `item`. `OnAVChange` / `OnResume` and the rest are the
*same* item and don't reset.

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

`PlayPause()` / `StopPlayback()` / `SeekStep()` / `SeekPercent()` /
`SetSpeed()` / `VolumeStep()` / `SetVolume()` / `ToggleMute()` /
`SendInput()` / `OpenLibraryItem()` are safe to call from any thread.
They queue the intent onto the connection loop, which owns the socket,
and are **fire-and-forget** - Kodi's own pushed notification, not the
reply, is what refreshes the snapshot; the reply frame is discarded by
the notification pump.

`SeekStep(forward)` and `VolumeStep(up)` send Kodi's own relative step
verbs (`Player.Seek` with `"smallforward"`/`"smallbackward"`,
`Application.SetVolume` with `"increment"`/`"decrement"`), so rapid
repeated taps of the Touch UI's ± buttons stack server-side.
`SeekPercent()` / `SetVolume()` are the absolute primitives underneath;
nothing in the Touch UI computes an absolute target from a snapshot
that only refreshes on a Kodi push.

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

### Library browsing (M4b)

Five buttons below Remote, visible whenever Kodi is connected (not gated
on anything currently playing): `KodiMoviesScreen`, `KodiTvShowsScreen`,
`KodiMusicScreen`, `KodiFilesScreen` and `KodiLiveTvScreen`
(`src/ui/screens/`), all reached from `NowPlayingScreen`.

Unlike every command above, a library browse is request/response, not
fire-and-forget - `KodiClient::RequestMovies()`/`RequestTvShows()`/
`RequestSeasons(tvshowid)`/`RequestEpisodes(tvshowid, season)` each queue
onto the connection loop, which issues the matching
`VideoLibrary.Get*` call via `Call()` (not the notification-driven push
path everything else on this page relies on - a library listing has no
push notification of its own) and publishes the parsed result as
`KodiMoviesFetchedEvent`/`KodiTvShowsFetchedEvent`/
`KodiSeasonsFetchedEvent`/`KodiEpisodesFetchedEvent`. These share
`pending_commands_`' bounded/drop-oldest queue shape
(`pending_library_requests_`, `kMaxPendingLibraryRequests`) but not its
fire-and-forget send: a query is worthless without its reply, so
`SendPendingLibraryRequests()` treats a failed/timed-out `Call()` as
fatal to the whole batch, the same as a dead transport anywhere else in
this module.

Listings are fetched through `CallLibrary()`: a 30 s timeout (a cold
network share can list slowly) and, for every list except file sources and
channel groups, pages of 500 items via Kodi's `limits` parameter, merged
into the one event the screen receives. Paging keeps each reply far below
`kMaxWebSocketMessageBytes` (1 MiB), which closes the connection when
exceeded, so library size is bounded by `kMaxLibraryItems` (10,000; the
list is truncated beyond that), not by the frame cap. A listing that
stops short - at that cap, or because a later page failed - sets
`truncated` on its `Kodi*FetchedEvent`, and the screen shows "Only the first
N items are shown." below the rows. A Kodi that answers
`limits` with a JSON-RPC error gets one unpaged retry. Every browse list is
rendered by `VirtualList`, which binds only the rows near the screen, so a
10,000-item list costs what one screenful does (see
[ui.md](ui.md#long-lists)).

A JSON-RPC `error` reply to a listing (for example PVR disabled on that
Kodi) parses to an empty list, so the screen shows its "Nothing here."
text rather than an error; only a transport failure or timeout is
reported as a connection problem.

`KodiMoviesScreen` is list-then-detail (movie list, then a selected
movie's Play/Resume choice), matching `DevicesScreen`'s own no-back-stack
shape. `KodiTvShowsScreen` is four levels deep (shows -> seasons ->
episodes -> an episode's Play/Resume choice); each screen's own "back"
button goes up exactly one level, never straight to the top or Home.
Resume is offered only when Kodi's own `resume` property on a movie/
episode has a non-zero position - `KodiMovie`/`KodiEpisode`'s
`resume_position_ms`, converted from that property's own `{position,
total}`-in-seconds shape (distinct from `Player.GetProperties`' `time`/
`totaltime` objects, which `MillisFromTimeObject()` parses instead).
Playback always starts through the existing `OpenLibraryItem()`
(`Player.Open`), landing on `NowPlayingScreen` immediately after.

Each screen requests its own top-level list on construction and again on
every transition into `KodiConnectionState::kConnected` (covers the
first connect and any later reconnect while the screen exists); a
show's seasons/episodes aren't known until that show/season is chosen,
so those queries fire only when that level is entered. Kodi's database
order has no relation to how a user browses, so every query sorts by
label (`VideoLibrary.GetMovies`/`GetTVShows`) or by season/episode
number, not left at Kodi's own insertion order.

`KodiMusicScreen` follows the video screens' `RequestX()`/
`KodiXFetchedEvent` pattern for its own `AudioLibrary.GetArtists`/
`GetAlbums(artistid)`/`GetSongs(albumid)` - Artists -> Albums -> Songs,
one level shallower than TV Shows. It has no fourth Play/Resume detail
level: `AudioLibrary.GetSongs` has no `resume` property at all
(requesting one is rejected with "Invalid params"), so a song plays directly on tap, the same
`OpenLibraryItem("songid", id, /*resume=*/false)` call with a different
`id_field` string. `KodiSong::duration_seconds` (Kodi's own `duration`
property, plain seconds - not the `{position, total}` shape movie/
episode `resume` carries) formats through the same `FormatKodiClock()`
`NowPlayingScreen`'s time label uses.

`KodiFilesScreen` browses the raw filesystem instead of scraped library
metadata - the fallback view Kodi's own Videos > Files menu offers for
content the library hasn't (or can't) match, e.g. home videos or a
folder with unrecognized naming. `RequestFileSources()` lists
`Files.GetSources("video")` (the configured source list); tapping a
folder pushes it and calls `RequestDirectory(path)`
(`Files.GetDirectory(path, "video")`) - both publish
`KodiFilesFetchedEvent`. Folder depth is unbounded here, unlike every
fixed-depth screen above, so the screen keeps its own `path_stack_` of
`{path, label}` and reuses one list container rather than allocating a
sibling per level; "back" pops one entry, or returns to the source list
once the stack is empty (hiding the back button - nothing is above the
source list). A source item carries no `filetype` field at all
since it's a folder by
definition, unlike a `Files.GetDirectory` item, which always has one;
`ParseFileItems()`'s `all_folders` parameter is what tells the two
shapes apart. A tapped file plays directly via `PlayFile()` (`Player.Open`
with a raw `{"file": path}` item, not a library id) - same no-Play/
Resume-choice reasoning as music.

`KodiLiveTvScreen` is the shallowest browse screen - channel groups
(`RequestChannelGroups()`, `PVR.GetChannelGroups` scoped to
`channeltype: "tv"`; radio, a separate PVR channel type, isn't browsed
here) then channels (`RequestChannels(channelgroupid)`,
`PVR.GetChannels`), publishing `KodiChannelGroupsFetchedEvent`/
`KodiChannelsFetchedEvent`. There's nothing to scope a channel by beyond
its group, so it stops at two levels rather than three or four. A live
broadcast has no resume point, so a tapped channel plays directly - not
through a new command, but the existing
`OpenLibraryItem("channelid", id, /*resume=*/false)` (`Player.Open`
accepts `channelid` as a valid item identifier alongside `movieid`/
`episodeid`/`songid`).

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
the two Web UI routes and the settings page. Implemented for M4b: movie,
TV show/season/episode, artist/album/song, raw-filesystem, and live-TV
channel-group/channel library browsing (see Library browsing above) -
video/music/files/live-TV, the full set the roadmap's Media browsing
item names. The `VideoLibrary.GetMovies`/`GetTVShows`/`GetSeasons`/
`GetEpisodes`, `AudioLibrary.GetArtists`/`GetAlbums`/`GetSongs`,
`Files.GetSources`/`GetDirectory`, and `PVR.GetChannelGroups`/
`GetChannels` response shapes `KodiClient` parses match a live Kodi 21
instance field-for-field.

`KodiClient`'s connect/reconcile/notification loop and its
discovery/selection policy are host-tested against fake `MdnsBrowser` /
`WebSocketClient` doubles plus one test over a libcurl-backed
`HostWebSocketClient` and a raw-socket loopback JSON-RPC peer
(`tests/kodi_client_test.cpp`), which also covers the eleven
`VideoLibrary.Get*`/`AudioLibrary.Get*`/`Files.Get*`/`PVR.Get*`
request/parse/publish paths and the queued-while-disconnected case. The
`MdnsBrowser` backend adapters themselves are not unit-tested and the
firmware one is on-device only — see
[networking.md](networking.md#status) for why, and for which part of M4
that verification belongs to.

`NowPlayingScreen` / `KodiRemoteScreen` / `KodiMoviesScreen` /
`KodiTvShowsScreen` / `KodiMusicScreen` / `KodiFilesScreen` /
`KodiLiveTvScreen`'s populated content (the transport row with
`SetTransportGlyph()`'s double-triangle rewind/fast-forward glyph, the
volume row, the D-pad, and the movie/show/season/episode/artist/album/
song/file/channel lists) each lives inside a `content_`/list container
that stays hidden until `KodiClient::Snapshot().state == kConnected`
(see `NowPlayingScreen::Refresh()` and each browse screen's own
`Refresh()`). In the simulator that state needs either a Kodi instance
on the LAN or the "Test: toggle fake Kodi connection" debug control (see
[simulator.md](simulator.md#how-it-works),
`simulator/debug_kodi_backend.cpp`, which also cans a two-movie/
one-show/one-artist/one-source/one-channel-group library so every browse
screen renders something once armed).

**Not built:** continue-watching/recently-added lists (resume is a
per-item choice only - see Library browsing above) and artwork, which
resolves only through Kodi's HTTP endpoint on the authenticated port 8080
([ADR-0030](../decisions/ADR-0030-kodi-jsonrpc-transport.md)). Both are
M7 items in [roadmap.md](../roadmap.md).
