# src/

The portable Core/UI/module source shared by [../firmware/](../firmware/)
and [../simulator/](../simulator/) — see
[docs/architecture/overview.md](../docs/architecture/overview.md#hardware-abstraction).
Code here must never call M5Unified/M5GFX/ESP-IDF or SDL2/LVGL-driver
APIs directly; each target links its own hardware-facing implementation
against the interfaces declared here.

```
src/
├── platform/    Task, Queue<T>, Timer - the Core Concurrency Abstraction
│                (see ADR-0002); host/ holds the shared std::thread-backed
│                implementation used by both simulator/ and tests/. A
│                firmware/platform/ FreeRTOS backend doesn't exist yet -
│                deliberately deferred until the on-device LVGL app
│                actually needs it, a separate roadmap item. Also
│                BatteryReader/TimeSource - small virtual interfaces
│                (not pImpl'd like Task/Timer - simple, rarely-called,
│                and directly mockable matters more than dispatch cost).
├── core/        EventBus - publish/subscribe with reference-counted
│                payloads (see ADR-0011). Deliberately has no LVGL
│                dependency, so it's fully unit-testable in tests/. Also
│                Clock - Time/date services, publishing a ClockTickEvent
│                once a second (and once immediately at construction).
└── ui/          UiTask - owns LVGL exclusively (the SDL2 window here,
                 M5GFX on firmware later), provides the lv_async_call()
                 hand-off EventBus's UI-facing subscriptions use. Also
                 Navigation - a minimal real route registry (Core's
                 Navigation responsibility conceptually, but lives here
                 since lv_scr_load() is a UI-layer implementation detail,
                 same reasoning as EventBus staying LVGL-free);
                 home_affordance.h - the reusable persistent home icon
                 every non-dashboard screen includes; and ui/screens/ -
                 DashboardScreen, the home screen (see
                 docs/architecture/dashboard.md).
```

`homedeck_ui` (the `ui/` target) is only defined when a target named
`lvgl` already exists in the including project's scope — `simulator/`
fetches LVGL before adding this directory, `tests/` never does, so
`tests/` only gets `homedeck_core`/`homedeck_platform_host`, keeping Core
logic testable without linking LVGL at all.

No module code exists yet — modules arrive in M3, built against whatever
concrete needs the Harmony module (the reference module, per
[ADR-0003](../docs/decisions/ADR-0003-module-architecture.md)) actually
has, not designed speculatively ahead of it.
