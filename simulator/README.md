# simulator/

The desktop simulator build target — runs the same Core/UI/module source as
[../firmware/](../firmware/) natively on Linux/macOS/Windows via LVGL's
SDL2 backend, for fast UI iteration without flashing hardware.

The host-native CMake project exists and builds: `cmake -B build -G Ninja &&
cmake --build build` produces a binary that opens a real 1280x720 SDL2
window. LVGL is pinned via CMake `FetchContent` to release `v9.5.0`.

Links against the portable Core/UI source in [../src/](../src/) — the
Core Concurrency Abstraction, `EventBus`, and the dedicated UI task all
run for real here: `main.cpp` wires up a background `Timer` that
publishes a `HeartbeatEvent` once a second, delivered safely to the UI
task via the `EventBus`'s `SubscribeUi`/`lv_async_call()` hand-off, and
rendered by `screens/heartbeat_screen.*` — a deliberately trivial proof
of the mechanism, not real product UI (see
[docs/roadmap.md](../docs/roadmap.md), M1). The dashboard shell replaces
it next. See [docs/architecture/simulator.md](../docs/architecture/simulator.md)
for the design, and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for the backend decision.
