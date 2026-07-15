# simulator/

The desktop simulator build target — runs the same Core/UI/module source as
[../firmware/](../firmware/) natively on Linux/macOS/Windows via LVGL's
SDL2 backend, for fast UI iteration without flashing hardware.

The host-native CMake project exists and builds: `cmake -B build -G Ninja &&
cmake --build build` produces a binary that opens a real 1280x720 SDL2
window and renders one LVGL label, confirming the CMake + LVGL + SDL2
combination works — the specific risk ADR-0002 and simulator.md flagged as
unverified. LVGL is pinned via CMake `FetchContent` to release `v9.5.0`.

No Core, UI, or module source exists here yet — `main.cpp` is a throwaway
scaffold, replaced once the dashboard shell and the shared Core/UI source
layout are built (see [docs/roadmap.md](../docs/roadmap.md), M1). See
[docs/architecture/simulator.md](../docs/architecture/simulator.md) for the
design, and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for the backend decision.
