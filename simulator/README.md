# simulator/

The desktop simulator build target — runs the same Core/UI/module source as
[../firmware/](../firmware/) natively on Linux/macOS/Windows via LVGL's
SDL2 backend, for fast UI iteration without flashing hardware.

The host-native CMake project exists and builds: `cmake -B build -G Ninja &&
cmake --build build` produces a binary that opens a real SDL2 window at
LVGL's logical resolution of 720x1280 - the Tab5 panel's native portrait
orientation, not the 1280x720 landscape figure on its spec sheet (see
[hardware.md](../docs/architecture/hardware.md#display-driver-strategy)).
The window itself renders smaller than that on-screen (`UiTask`'s `zoom`
parameter, a desktop dev-convenience so a 1280px-tall canvas doesn't
demand that much vertical monitor space) — LVGL still lays out at the
full logical resolution underneath. Override it per-machine with the
`HOMEDECK_SIM_ZOOM` environment variable rather than editing
`main.cpp`'s compiled-in default (currently `0.75`, chosen for clean
integer window dimensions: `540x960`) — there's no single
value that fits every desktop/taskbar layout:

```sh
HOMEDECK_SIM_ZOOM=0.55 ./build/homedeck_simulator
```

**Known trade-off, not a bug:** LVGL's SDL backend softens text at any
zoom other than `1.0`. `UiTask` sets `SDL_HINT_RENDER_SCALE_QUALITY` to
`"best"` (anisotropic filtering, where the renderer supports it), which
measurably reduces but doesn't eliminate this on a real GPU-accelerated
desktop — software renderers (Xvfb, CI, remote desktops without GPU
passthrough) generally can't do anisotropic filtering and silently fall
back to plain linear scaling regardless of the hint, so don't trust a
headless comparison of scale-quality settings. The only setting that
fully eliminates the softening is `HOMEDECK_SIM_ZOOM=1.0` (full
sharpness, but the window then exceeds a typical 1080p desktop's usable
height). Real hardware is unaffected either way; it always renders
LVGL's own anti-aliasing untouched, at native resolution, with no
scaling step. LVGL is pinned via CMake `FetchContent` to release
`v9.5.0`.

Links against the portable Core/UI source in [../src/](../src/) —
`main.cpp` is wiring: `DashboardScreen` (the real home screen — see
[docs/architecture/dashboard.md](../docs/architecture/dashboard.md)) and
Core/platform services, host-backed where firmware would use real
hardware. See [docs/roadmap.md](../docs/roadmap.md) for what's currently
wired in, [docs/architecture/simulator.md](../docs/architecture/simulator.md)
for the design, and
[ADR-0002](../docs/decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for the backend decision. `screens/placeholder_screen.h` is a
deliberately minimal second screen proving Navigation and the persistent
home affordance (`src/ui/home_affordance.h`) work, until a genuine
second screen replaces it.
