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

## Test doubles and real backends

- Concurrency (`Task`/`Queue`/`Timer`), `HostHttpServer`, `HostHttpClient`
  and `HostWebSocketClient` are exercised against the implemented host
  backend: a queue blocking and delivering in FIFO order, a timer firing on
  schedule, a request/response round trip over a raw socket.
- Module tests (`HarmonyConnection`, `KodiClient`) run mainly against
  scriptable `WebSocketClient`/`MdnsBrowser`/`HttpClient` doubles, plus
  real-backend tests against a raw-socket stand-in peer for the timing
  behaviour a double cannot reproduce.
- Every Web UI route has a `*_routes_test.cpp` that drives its
  authentication, input-validation and error paths over the same socket
  `HostHttpServer` uses.
- Logic that must be testable without LVGL or ESP-IDF is kept in portable
  units (`src/core/wifi_reconnect_policy.h`, `src/core/wifi_credentials.h`,
  `src/ui/lazy_load.h`, `src/ui/virtual_list_window.h`, ...) and tested
  here. The rest of `firmware/main/wifi_setup.cpp`, the firmware platform
  backends, `HostMdnsBrowser` and the LVGL screens are not reachable from
  this suite; they are verified in the simulator and on hardware.

## Test files

| File | Covers |
|---|---|
| `smoke_test.cpp` | The framework builds, links and runs |
| `task_test.cpp`, `queue_test.cpp`, `timer_test.cpp` | Core Concurrency Abstraction |
| `event_bus_test.cpp` | `EventBus`, including `SubscribeUi` routing through an injected dispatcher |
| `clock_test.cpp`, `time_format_test.cpp`, `text_format_test.cpp` | `Clock` (immediate tick at construction), time and string formatting helpers |
| `storage_test.cpp` | `Storage` tiers and per-module namespacing |
| `battery_reader_test.cpp`, `display_brightness_test.cpp` | Host `BatteryReader` and `DisplayBrightness` |
| `latched_threshold_monitor_test.cpp`, `low_battery_monitor_test.cpp`, `critical_battery_monitor_test.cpp` | The shared latch state machine and the two monitors built on it |
| `network_status_monitor_test.cpp`, `notification_sound_test.cpp` | Network status events, notification sound |
| `power_manager_test.cpp` | `Active`/`Idle`/`Sleeping`/`Updating`/`Error` transitions, sleep veto, brightness clamping |
| `ota_gate_test.cpp` | The OTA battery/power gate |
| `host_validation_test.cpp`, `json_request_test.cpp` | Host-string screening and the JSON nesting-depth bound shared by both modules and the routes |
| `retry_backoff_test.cpp` | `RetryBackoff`, used by both modules |
| `wifi_reconnect_policy_test.cpp`, `wifi_credentials_test.cpp`, `url_codec_test.cpp` | The portable decision logic behind `wifi_setup.cpp` and query/form parsing |
| `grid_occupancy_test.cpp`, `activity_start_tracker_test.cpp`, `command_button_press_tracker_test.cpp` | LVGL-free logic of the dashboard grid, `ActivitiesScreen` and `DevicesScreen` |
| `http_server_test.cpp`, `http_client_test.cpp`, `static_assets_test.cpp` | Host HTTP server and client, static asset serving |
| `websocket_client_test.cpp` | `HostWebSocketClient` against a raw-socket RFC 6455 server: reassembly across reads and frames, ping/pong, close echo, reserved-opcode rejection, `Sec-WebSocket-Accept` validation, the message-size cap, `ReceiveText(0)`, and `IsOpen()` (a timeout before a frame leaves the link open; a timeout mid-frame or a peer close closes it) |
| `logger_test.cpp` | Structured logging and async persistence |
| `weather_provider_test.cpp`, `weather_routes_test.cpp` | `OpenMeteoWeatherProvider` against an `HttpClient` double, and its routes |
| `admin_auth_service_test.cpp`, `admin_auth_routes_test.cpp` | Admin password, sessions and their routes |
| `settings_routes_test.cpp`, `ota_routes_test.cpp`, `diagnostics_routes_test.cpp`, `wifi_routes_test.cpp` | The matching Web UI routes |
| `harmony_connection_test.cpp` | `HarmonyConnection`: connect/retry/liveness, the press/hold/release command path and its queue, plus two real-backend tests (`HostHttpClient`/`HostWebSocketClient` against a stand-in hub) |
| `harmony_notification_bridge_test.cpp`, `harmony_routes_test.cpp` | Notify-once-per-outage latch; Harmony routes |
| `kodi_client_test.cpp` | `KodiClient`: see below |
| `kodi_routes_test.cpp` | `/api/kodi/status` and `/api/kodi/reconnect`: authentication and snapshot serialisation, including non-UTF-8 discovered strings |
| `kodi_display_test.cpp` | The Kodi widget line, Now Playing subtitle and clock formatting |
| `virtual_list_window_test.cpp` | `ComputeVisibleRows()`, the arithmetic behind `VirtualList` |
| `lazy_load_test.cpp` | `LazyLoad`, which decides when a browse screen requests its list, including the retry after a failed load |

### `kodi_client_test.cpp`

Against fake `MdnsBrowser`/`WebSocketClient` doubles:

- Discovery and selection: manual host override, saved `uuid`, single-instance
  auto-select, ambiguous, offline and address-less instances, unsafe hosts.
- The JSON-RPC `id`-correlation loop with interleaved notifications.
- Notification-driven Now Playing state, `can_seek`, idle or paused
  reconcile polls not republishing an unchanged snapshot, and identity
  being re-read after a reconnect.
- The command surface: playback, `Input.*` and `Player.Open`, relative seek
  and volume steps, active-player resolution, stale-command dropping with
  stop kept, the `kMaxPendingCommands` cap, and the `kMaxPumpIterations`
  drain bound.
- All eleven library queries: request params, parsing, the published event,
  paging, the unpaged retry, the `kMaxLibraryItems` cap, the `truncated`
  flag for a failed later page and for a timeout (which keeps the link),
  and commands sent between pages.
- Timeouts on an open link: a library listing or a reconcile poll that
  times out keeps the connection, except that repeated unanswered polls
  reconnect.
- The `library_busy` flag: raised by a slow library reply, cleared by any
  reply (including a late one read while idle) and on disconnect, and never
  raised by a fast reply.
- Type-mismatched fields and wrongly typed notifications falling back to
  defaults instead of crashing.

`RealBackendConnectsReconcilesAndHandlesAPushedNotification` drives the
libcurl-backed `HostWebSocketClient` against a raw-socket loopback JSON-RPC
peer.

## Build and run locally

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Runs in CI on every push/PR — see
[../.github/workflows/tests.yml](../.github/workflows/tests.yml).
