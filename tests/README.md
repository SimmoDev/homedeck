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
`queue_test.cpp`, `timer_test.cpp`), `EventBus`
(`event_bus_test.cpp`), `Clock` (`clock_test.cpp` — including the
immediate-tick-at-construction behavior, a real bug this test caught),
and the host `BatteryReader` (`battery_reader_test.cpp`) for real — a
queue actually blocking and delivering in FIFO order, a timer actually
firing on schedule and stopping promptly on destruction, `SubscribeUi`
actually routing through an injected dispatcher — plus the original
`smoke_test.cpp` proving the framework itself builds, links, and runs.
Real module tests arrive alongside the modules they test, not before
they exist.

Build and run locally:

```
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Runs in CI on every push/PR — see
[../.github/workflows/tests.yml](../.github/workflows/tests.yml).
