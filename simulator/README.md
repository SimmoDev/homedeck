# simulator/

The desktop simulator build target — runs the same Core/UI/module source as
[../firmware/](../firmware/) natively on Linux/macOS/Windows via LVGL's
SDL2 backend, for fast UI iteration without flashing hardware.

The host-native CMake project exists and builds: `cmake -B build -G Ninja &&
cmake --build build` produces a binary that opens a real 1280x720 SDL2
window. LVGL is pinned via CMake `FetchContent` to release `v9.5.0`.

Links against the portable Core/UI source in [../src/](../src/) — `main.cpp`
is now just wiring: `Clock`, `HostBatteryReader`, `DashboardScreen` (the
real home screen — see
[docs/architecture/dashboard.md](../docs/architecture/dashboard.md)), and
`Navigation`. `screens/placeholder_screen.h` is this milestone's version
of the earlier throwaway heartbeat screen — deliberately minimal,
existing only to prove Navigation and the persistent home affordance
(`src/ui/home_affordance.h`) actually work, until a genuine second screen
replaces it. See
[docs/architecture/simulator.md](../docs/architecture/simulator.md) for
the design, and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for the backend decision.
