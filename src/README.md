# src/

The portable Core/UI/module source shared by [../firmware/](../firmware/)
and [../simulator/](../simulator/) — see
[docs/architecture/overview.md](../docs/architecture/overview.md#hardware-abstraction).
Code here must never call ESP-IDF, the `espressif/m5stack_tab5` BSP, or
SDL2/LVGL-driver APIs directly; each target links its own hardware-facing
implementation against the interfaces declared here. (Not M5Unified/M5GFX
specifically — see
[ADR-0014](../docs/decisions/ADR-0014-hardware-support-library.md) for
why firmware's actual hardware support library differs from CLAUDE.md's
originally-named one.)

```
src/
├── platform/    Task, Queue<T>, Timer - the Core Concurrency Abstraction
│                (see ADR-0002); host/ holds the shared std::thread-backed
│                implementation used by both simulator/ and tests/;
│                firmware/ holds the FreeRTOS-backed one
│                (xTaskCreate/xTimerCreate directly), plus BatteryReader/
│                TimeSource implementations reading real hardware (the
│                INA226 power monitor, the RX8130CE RTC - see
│                docs/architecture/hardware.md#on-device-dashboard).
│                Queue<T>'s firmware backend stays deferred - still
│                nothing uses it. BatteryReader/TimeSource themselves are
│                small virtual interfaces (not pImpl'd like Task/Timer -
│                simple, rarely-called, and directly mockable matters
│                more than dispatch cost).
├── core/        EventBus - publish/subscribe with reference-counted
│                payloads (see ADR-0011). Deliberately has no LVGL
│                dependency, so it's fully unit-testable in tests/. Also
│                Clock - Time/date services, publishing a ClockTickEvent
│                once a second (and once immediately at construction).
└── ui/          UiTask - owns LVGL exclusively via the SDL2 window on
                 the simulator; firmware has no equivalent class, since
                 `espressif/m5stack_tab5`'s `bsp_display_start()` already
                 owns LVGL on-device (see
                 docs/architecture/hardware.md#on-device-dashboard) - only
                 the `lv_async_call()` hand-off pattern itself is
                 replicated in firmware/main/homedeck.cpp, not the whole
                 class. Also Navigation - a minimal real route registry
                 (Core's Navigation responsibility conceptually, but lives
                 here since lv_scr_load() is a UI-layer implementation
                 detail, same reasoning as EventBus staying LVGL-free);
                 home_affordance.h - the reusable persistent home icon
                 every non-dashboard screen includes; and ui/screens/ -
                 DashboardScreen, the home screen (see
                 docs/architecture/dashboard.md), reused directly by both
                 the simulator and firmware.
```

`homedeck_ui` (the `ui/` target) is only defined when a target named
`lvgl` already exists in the including project's scope — `simulator/`
fetches LVGL before adding this directory, `tests/` never does, so
`tests/` only gets `homedeck_core`/`homedeck_platform_host`, keeping Core
logic testable without linking LVGL at all. `firmware/` doesn't use this
mechanism at all - its CMakeLists.txt lists the handful of reused `src/`
files it needs directly, rather than nesting this plain-CMake build
inside ESP-IDF's own component system (see
docs/architecture/hardware.md#on-device-dashboard for why).

No module code exists yet — modules arrive in M3, built against whatever
concrete needs the Harmony module (the reference module, per
[ADR-0003](../docs/decisions/ADR-0003-module-architecture.md)) actually
has, not designed speculatively ahead of it.
