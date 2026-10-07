# src/

The portable Core/UI/module source shared by [../firmware/](../firmware/)
and [../simulator/](../simulator/) — see
[docs/architecture/overview.md](../docs/architecture/overview.md#hardware-abstraction).
Code here must never call ESP-IDF, the `espressif/m5stack_tab5` BSP, or
SDL2/LVGL-driver APIs directly; each target links its own hardware-facing
implementation against the interfaces declared here. (Firmware's hardware
support library is not M5Unified/M5GFX - see
[ADR-0014](../docs/decisions/ADR-0014-hardware-support-library.md).)

```text
src/
├── platform/     Hardware- and OS-facing interfaces, one small virtual
│                 class each (Task, Queue, Timer, the settings/secret/cache
│                 stores, HTTP server and client, WebSocket client, mDNS
│                 browser, battery, time, audio, display brightness, ...).
│                 host/ implements them for the simulator and tests,
│                 firmware/ for the Tab5. A few helpers are portable and
│                 sit beside the interfaces (static_assets, the WebSocket
│                 message assembler, steady_time_source).
├── core/         Services with no LVGL dependency, so they are fully
│                 unit-testable in tests/: EventBus, Clock,
│                 Storage, Logger, notifications and their monitors,
│                 PowerManager, AdminAuthService, the weather provider,
│                 the Harmony and Kodi modules (HarmonyConnection,
│                 KodiClient), and the Web UI route registration files,
│                 one per API surface.
├── ui/           LVGL code: AppCore (the composition root shared by both
│                 entry points), Navigation (here rather than core/
│                 because it calls lv_scr_load()), the dashboard and its
│                 widgets, the status bar and banner, VirtualList and
│                 ScreenLoader, and ui/screens/ (every full-screen view,
│                 Harmony's and Kodi's included).
└── third_party/  Vendored header-only dependencies (see below).
```

[docs/architecture/core.md](../docs/architecture/core.md#status) lists the
Core services, [modules.md](../docs/architecture/modules.md#status) each
module's files, screens and widget, and the other
[architecture docs](../docs/architecture/) the rest.

`third_party/` holds header-only dependencies that need to be visible
identically to both build systems (see
[third_party/README.md](third_party/README.md)).

`homedeck_ui` (the `ui/` target) is only defined when a target named
`lvgl` already exists in the including project's scope — `simulator/`
fetches LVGL before adding this directory, `tests/` never does, so
`tests/` only gets `homedeck_core`/`homedeck_platform_host`, keeping Core
logic testable without linking LVGL at all. `firmware/` doesn't use this
mechanism at all - its CMakeLists.txt lists the handful of reused `src/`
files it needs directly, rather than nesting this plain-CMake build
inside ESP-IDF's own component system (see
`firmware/main/CMakeLists.txt`'s own comment for why).

`core/module.h` is the module lifecycle contract (`Start()`/`Stop()`, construction
as Init, the destructor as teardown) - see
[ADR-0003](../docs/decisions/ADR-0003-module-architecture.md).
