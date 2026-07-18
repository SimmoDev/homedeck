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
│                nothing uses it. BatteryReader/TimeSource/SettingsStore/
│                SecretStore/CacheStore are small virtual interfaces (not
│                pImpl'd like Task/Timer - simple, infrequently-called,
│                and directly mockable matters more than dispatch cost).
│                SettingsStore/SecretStore/CacheStore back Storage's
│                tiers (see core/ below); host/ implements all three as
│                real files under a caller-supplied scratch directory
│                (SecretStore under a separate `secrets/` subdirectory
│                from SettingsStore's `settings/`, so the two never
│                collide on disk), firmware/ wraps NVS (SettingsStore,
│                SecretStore - same partition and per-`ns` namespace as
│                each other, a documented caller constraint rather than a
│                structural guarantee, see
│                platform/firmware/secret_store.h) and the `storage` FAT
│                partition (CacheStore, see ADR-0017). Also HttpServer -
│                the Web Management UI's server primitive (see
│                docs/architecture/web-ui.md#status), another small
│                virtual interface; host/ implements it with civetweb
│                (HostHttpServer), firmware/ wraps esp_http_server
│                (FirmwareHttpServer, see
│                ADR-0002#3-embedded-webwebsocket-server). Carries a
│                Cookie request header and arbitrary extra response
│                headers (not a generic header map - see
│                http_server.h's own comment for why), needed for
│                AdminAuthService's session cookie. Also
│                SteadyTimeSource - a portable (no host/firmware split)
│                TimeSource backed by std::chrono::steady_clock rather
│                than a wall clock, for callers that need reliable
│                elapsed-time comparisons rather than a calendar date
│                (AdminAuthService's session expiry, since firmware's
│                RTC-backed TimeSource has no guaranteed
│                call-to-call consistency until ADR-0016's
│                never-calibrated-RTC gap is fixed).
├── core/        EventBus - publish/subscribe with reference-counted
│                payloads (see ADR-0011). Deliberately has no LVGL
│                dependency, so it's fully unit-testable in tests/. Also
│                Clock - Time/date services, publishing a ClockTickEvent
│                once a second (and once immediately at construction).
│                Also Storage - Core's Configuration and Storage
│                responsibilities in one class (see core.md#status):
│                schema-versioned settings/secret/cache read-write over
│                platform/'s SettingsStore/SecretStore/CacheStore,
│                namespaced per module by requiring a module ID on every
│                call. NVS encryption and the microSD tier are
│                deliberately not built yet (see ADR-0018/ADR-0012). Also
│                notification.h/LowBatteryMonitor - the Notifications
│                service's urgency-tagged event and its first real
│                publisher (see core.md#status). Also AdminAuthService -
│                the Web Management UI's admin authentication (see
│                web-ui.md#status): PBKDF2-SHA256 password hashing and
│                session-token generation via mbedtls (the same library
│                on both targets - FetchContent'd for host, ESP-IDF's own
│                vendored copy for firmware), mutex-guarded since it's
│                called from HTTP server worker threads, not the LVGL UI
│                task like most of Core.
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
                 every non-dashboard screen includes; StatusBar - the
                 persistent date/time/battery chrome every screen
                 constructs its own copy of; Widget/DashboardGrid - the
                 standard dashboard widget interface and its grid layout
                 (see dashboard.md#widget-system); NotificationBanner -
                 the screen-banner notification output, parented to
                 LVGL's top layer so it renders above whatever screen is
                 active; and ui/screens/ - DashboardScreen, the home
                 screen (see docs/architecture/dashboard.md), reused
                 directly by both the simulator and firmware.
```

`third_party/` sits alongside these three - vendored header-only dependencies
that need to be visible identically to both build systems (see
[third_party/README.md](third_party/README.md)).

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
