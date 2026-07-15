# simulator/

The desktop simulator build target — runs the same Core/UI/module source as
[../firmware/](../firmware/) natively on Linux/macOS/Windows via LVGL's
SDL2 backend, for fast UI iteration without flashing hardware.

The host-native CMake project exists and builds: `cmake -B build -G Ninja &&
cmake --build build` produces a binary that opens a real 1280x720 SDL2
window. LVGL is pinned via CMake `FetchContent` to release `v9.5.0`.

Links against the portable Core/UI source in [../src/](../src/) — `main.cpp`
is now just wiring: `Clock`, `HostBatteryReader`, and `DashboardScreen`
(the real home screen — see
[docs/architecture/dashboard.md](../docs/architecture/dashboard.md)),
which replaced the throwaway heartbeat proof-of-mechanism screen from the
previous M1 item now that the mechanism it proved (background `Timer` →
`EventBus` → `lv_async_call()` hand-off) has a real consumer. See
[docs/architecture/simulator.md](../docs/architecture/simulator.md) for
the design, and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for the backend decision.
