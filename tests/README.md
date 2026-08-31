# tests/

Test suites for HomeDeck: GoogleTest+GoogleMock unit tests, built as their
own host-native CMake project (the same tooling approach as
[../simulator/](../simulator/), not ESP-IDF, but a separate CMake project
from it). Links against `homedeck_core` from [../src/](../src/) — Core's
LVGL-free portion; `../src/`'s `homedeck_ui` target (which does depend on
LVGL) is never built here, since LVGL is never fetched in this project.
No separate on-target test framework is used — see
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#5-test-framework)
for why.

Covers the Core Concurrency Abstraction (`task_test.cpp`,
`queue_test.cpp`, `timer_test.cpp`), `EventBus` (`event_bus_test.cpp`),
`Clock` (`clock_test.cpp`, including the immediate-tick-at-construction
behavior), `Storage` (`storage_test.cpp`), `HostHttpServer`
(`http_server_test.cpp`, a real request/response round trip over a raw
socket), `LatchedThresholdMonitor` (`latched_threshold_monitor_test.cpp` -
the shared latch state machine `LowBatteryMonitor`/`CriticalBatteryMonitor`
are both built on), `LowBatteryMonitor` (`low_battery_monitor_test.cpp`), and
`AdminAuthService` (`admin_auth_service_test.cpp`,
`admin_auth_routes_test.cpp`) for real — a queue actually blocking and
delivering in FIFO order, a timer actually firing on schedule and
stopping promptly on destruction, `SubscribeUi` actually routing through
an injected dispatcher — plus `smoke_test.cpp` proving the framework
itself builds, links, and runs. Every other M2 Web UI endpoint has its
own equally real `*_routes_test.cpp` alongside it (`settings_routes_test.cpp`,
`ota_routes_test.cpp`, `diagnostics_routes_test.cpp`,
`weather_routes_test.cpp`, `wifi_routes_test.cpp`), each driving auth-required,
input-validation, and error-path cases over the same real socket
`HostHttpServer` uses;
`PowerManager` (`power_manager_test.cpp` — the full
Active/Idle/Sleeping/Updating/Error state machine, the sleep-veto
mechanism, `Updating`/`Error` precedence over each other and over
inactivity timeouts, and active-brightness clamping/baseline behavior);
`static_assets_test.cpp`,
`critical_battery_monitor_test.cpp`, `notification_sound_test.cpp`,
`network_status_monitor_test.cpp`, `weather_provider_test.cpp`,
`grid_occupancy_test.cpp`, `time_format_test.cpp`,
`text_format_test.cpp` (`SplitCamelCase()`, `src/ui/text_format.h` - a
pure string helper despite living under `src/ui/`, same reasoning as
`time_format_test.cpp`), `activity_start_tracker_test.cpp`
(`ActivityStartTracker`, `src/ui/activity_start_tracker.h` -
`ActivitiesScreen`'s optimistic "Starting <name>..." status-tracking
decision logic, pulled out for the same reason),
`command_button_press_tracker_test.cpp`
(`CommandButtonPressTracker`, `src/ui/command_button_press_tracker.h` -
`DevicesScreen`'s long-press/scroll-vs-tap decision logic, pulled out
for the same reason), `retry_backoff_test.cpp` (`RetryBackoff`,
`src/core/retry_backoff.h` - the generic exponential-backoff utility
`HarmonyConnection` is the first consumer of), `http_client_test.cpp`,
`logger_test.cpp`, `battery_reader_test.cpp`, `display_brightness_test.cpp`,
and `ota_gate_test.cpp` round out coverage for their respective
subsystems. `wifi_reconnect_policy_test.cpp` and `wifi_credentials_test.cpp` cover the
two pieces of `firmware/main/wifi_setup.cpp`'s decision logic that have
been pulled out into portable, LVGL/ESP-IDF-free units
(`src/core/wifi_reconnect_policy.h`, `src/core/wifi_credentials.h`)
specifically so they're testable here - the rest of that file is 100%
ESP-IDF-coupled and unreachable from this suite. `url_codec_test.cpp`
covers the percent-decoding shared by that same form-parsing path and
`weather_routes.cpp`'s GET query parsing (`src/core/url_codec.h`).

`harmony_connection_test.cpp` covers `HarmonyConnection` (M3, the first
module test) - connect/retry/liveness-probe behavior and the
press/hold/release device-command path, including the pending-command
queue's staleness/batch-failure drop semantics and a release command's
exemption from both - against a scriptable `WebSocketClient` double,
the same fake-transport-double approach
`weather_provider_test.cpp` already established for `HttpClient`, plus
two real-backend tests against real `HostHttpClient`/`HostWebSocketClient`
instances and a raw-socket stand-in hub instead - the fake-only coverage
above can't reproduce real socket/timing behavior, the same reasoning
`websocket_client_test.cpp`'s own top comment gives for testing
`HostWebSocketClient` against a real server rather than only in
isolation. `RealBackendConnectsHandshakesAndFetchesConfig
OverActualSocketsAndFraming` drives the full connect pipeline end to
end; `RealBackendDrainsAStaleFrameBeforeTheNextLivenessProbesOwnReceive`
covers the one timing-sensitive path the first test's own clean
request/reply sequence doesn't reach - `DrainStaleMessages()` actually
draining a frame that's already sitting in a real socket's receive
buffer, not just a fake double's own queue.
`websocket_client_test.cpp` covers `HostWebSocketClient`'s actual
libcurl-backed transport, which that double stands in for - a real
client/server WebSocket round trip over a raw socket against a
hand-rolled RFC 6455 server, the same "test for real, not mocked"
approach `http_server_test.cpp`/`http_client_test.cpp` already use for
HTTP, covering a single frame reassembled across multiple reads,
multi-frame continuation reassembly, ping/pong keepalive interleaved
with a fragmented message, a close frame echoed back, reserved/
unexpected opcode rejection, `Sec-WebSocket-Accept` handshake
validation, the message-size cap, and the zero-timeout non-blocking
`ReceiveText(0)` case.
`harmony_notification_bridge_test.cpp` covers `HarmonyNotificationBridge`'s
notify-once-per-outage latch, and `harmony_routes_test.cpp` covers its two
Web UI routes.

`kodi_client_test.cpp` covers `KodiClient` (M4, the second module test) -
mDNS instance discovery/selection (manual host override, saved-`uuid`
match, single-instance auto-select, and the ambiguous/offline cases that
must stay disconnected rather than guess), the JSON-RPC `id`-correlation
loop dispatching interleaved pushed notifications, notification-driven
Now Playing state (play/pause/stop, volume) with identity taken from the
notification rather than a later `Player.GetItem`, `can_seek` threaded
from the reconcile poll into the snapshot, an idle or paused reconcile
not republishing an unchanged snapshot, the "unreachable Kodi raises no
notification" path, and the fire-and-forget command surface
(playback/`Input.*`/`Player.Open` queued onto the connection thread, the
transport row's relative seek/volume steps sent as Kodi's own
`smallforward`/`increment` step verbs, the active-player-id resolved at
send time, stale non-exempt entries dropped on reconnect while a stop is
kept, the pending-command queue dropping its oldest entries past
`kMaxPendingCommands`, and `PumpNotifications()` stopping its drain at
`kMaxPumpIterations` rather than following an unbounded backlog) -
against fake `MdnsBrowser`/`WebSocketClient` doubles, plus
`RealBackendConnectsReconcilesAndHandlesAPushedNotification` driving the
libcurl-backed `HostWebSocketClient` against a raw-socket loopback
JSON-RPC peer, the same reasoning `harmony_connection_test.cpp` and
`websocket_client_test.cpp` give for testing against a genuine transport
rather than only a double. `kodi_routes_test.cpp` covers the two Web UI
routes (`/api/kodi/status`, `/api/kodi/reconnect`) - auth gating and the
`KodiSnapshot` wire serialisation, empty and populated.
`kodi_display_test.cpp` covers the LVGL-free display-string helpers the
Kodi Touch UI renders - the widget's status line for each connection/
playback state, the Now Playing subtitle (show + zero-padded `S03E07`
code, kept wide past 99 / movie title / bare-verb fallback / "Nothing
playing"), and the `m:ss` / `h:mm:ss` clock formatter.

Further module tests arrive alongside the modules they test, not before
they exist.

Build and run locally:

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Runs in CI on every push/PR — see
[../.github/workflows/tests.yml](../.github/workflows/tests.yml).
