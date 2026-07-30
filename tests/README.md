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
socket), `LowBatteryMonitor` (`low_battery_monitor_test.cpp`), and
`AdminAuthService` (`admin_auth_service_test.cpp`,
`admin_auth_routes_test.cpp`) for real — a queue actually blocking and
delivering in FIFO order, a timer actually firing on schedule and
stopping promptly on destruction, `SubscribeUi` actually routing through
an injected dispatcher — plus `smoke_test.cpp` proving the framework
itself builds, links, and runs. Every other M2 Web UI endpoint has its
own equally real `*_routes_test.cpp` alongside it (`settings_routes_test.cpp`,
`ota_routes_test.cpp`, `diagnostics_routes_test.cpp`,
`weather_routes_test.cpp`), each driving auth-required, input-validation,
and error-path cases over the same real socket `HostHttpServer` uses;
`static_assets_test.cpp`, `power_manager_test.cpp`,
`critical_battery_monitor_test.cpp`, `notification_sound_test.cpp`,
`network_status_monitor_test.cpp`, `weather_provider_test.cpp`,
`grid_occupancy_test.cpp`, `time_format_test.cpp`, `http_client_test.cpp`,
`logger_test.cpp`, and `battery_reader_test.cpp` round out coverage for
their respective subsystems. `wifi_reconnect_policy_test.cpp` and `wifi_credentials_test.cpp` cover the
two pieces of `firmware/main/wifi_setup.cpp`'s decision logic that have
been pulled out into portable, LVGL/ESP-IDF-free units
(`src/core/wifi_reconnect_policy.h`, `src/core/wifi_credentials.h`)
specifically so they're testable here - the rest of that file is 100%
ESP-IDF-coupled and unreachable from this suite. `url_codec_test.cpp`
covers the percent-decoding shared by that same form-parsing path and
`weather_routes.cpp`'s GET query parsing (`src/core/url_codec.h`). Real
module tests arrive alongside the modules they test, not before they
exist.

Build and run locally:

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Runs in CI on every push/PR — see
[../.github/workflows/tests.yml](../.github/workflows/tests.yml).
