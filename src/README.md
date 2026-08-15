# src/

The portable Core/UI/module source shared by [../firmware/](../firmware/)
and [../simulator/](../simulator/) — see
[docs/architecture/overview.md](../docs/architecture/overview.md#hardware-abstraction).
Code here must never call ESP-IDF, the `espressif/m5stack_tab5` BSP, or
SDL2/LVGL-driver APIs directly; each target links its own hardware-facing
implementation against the interfaces declared here. (Not M5Unified/M5GFX
specifically — see
[ADR-0014](../docs/decisions/ADR-0014-hardware-support-library.md) for
why firmware's actual hardware support library differs from [CLAUDE.md](../CLAUDE.md)'s
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
│                Queue<T>'s firmware backend is confirmed working - see
│                Logger (core/ below) and ADR-0020 for its first firmware
│                use. BatteryReader/TimeSource/SettingsStore/
│                SecretStore/CacheStore are small virtual interfaces (not
│                pImpl'd like Task/Timer - simple, infrequently-called,
│                and directly mockable matters more than dispatch cost).
│                SettingsStore/SecretStore/CacheStore back Storage's
│                tiers (see core/ below); host/ implements all three as
│                real files under a caller-supplied scratch directory
│                (SecretStore under a separate `secrets/` subdirectory
│                from SettingsStore's `settings/`, so the two never
│                collide on disk), firmware/ wraps NVS (SettingsStore
│                on the default partition, SecretStore on its own
│                dedicated `secrets` partition - see
│                docs/decisions/ADR-0027-secret-store-partition-separation.md)
│                and the `storage` FAT partition (CacheStore, see
│                ADR-0017). Also HttpServer -
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
│                static_assets.h/.cpp - ServeStaticFiles(), a portable
│                (no host/firmware split) helper registering one exact-
│                path GET handler per Web UI static asset via
│                RegisterHandler(); firmware/main/homedeck.cpp builds the
│                asset list from EMBED_FILES-linked flash data,
│                simulator/main.cpp reads webui/dist/ off disk once at
│                startup (see
│                docs/decisions/ADR-0025-webui-static-asset-storage.md for
│                why assets live in the app image, not a partition). Also
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
│                task like most of Core. Also the Web UI route
│                registration files, one per API surface: settings_routes
│                (the generic settings/backup REST API, with the
│                reserved-key guard keeping AdminAuthService's password
│                hash out of it - see ADR-0023), ota_routes (status/
│                upload/reboot, gated by ota_gate.h's battery/power check
│                - see ADR-0005), diagnostics_routes (reset reason, core
│                dump download, structured logs - see diagnostics.md),
│                and weather_routes (the location-search/save endpoints
│                proxying Open-Meteo's geocoding API). Also
│                weather_provider.h/.cpp - OpenMeteoWeatherProvider, the
│                pluggable WeatherProvider interface's direct
│                implementation (see ADR-0008), polling on its own
│                background Task. Also logger.h/.cpp - Logger, Core's
│                structured/leveled log (see ADR-0019/ADR-0020),
│                persisting asynchronously on its own background Task.
│                Also notification_sound.h/.cpp - NotificationSound, the
│                Notifications service's sound presentation, playing a
│                generated tone via AudioOutput on its own background
│                Task. Also network_status_monitor.h/.cpp -
│                NetworkStatusMonitor, republishing NetworkStatus's
│                connectivity transitions as Notifications-facing events.
└── ui/          UiTask - owns LVGL exclusively via the SDL2 window on
                 the simulator; firmware has no equivalent class, since
                 `espressif/m5stack_tab5`'s `bsp_display_start()` already
                 owns LVGL on-device - only the `lv_async_call()`
                 hand-off pattern itself is replicated in
                 firmware/main/homedeck.cpp, not the whole class. Also
                 Navigation - a minimal route registry
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
                 active. Also the dashboard widgets: ClockWidget
                 (the large clock tile, distinct from StatusBar's compact
                 one), NetworkStatusWidget (SSID/IP detail beyond the
                 status bar's Wi-Fi icon), WeatherWidget (see
                 dashboard.md#weather-source), and NotificationWidget
                 (a last-notification tile, not an unread-count badge -
                 see roadmap.md's M7 list for that variant). Also
                 keyboard_input.h/.cpp - OnScreenKeyboard, a reusable
                 touch keyboard any screen can attach to its own
                 lv_textarea. And ui/screens/ - DashboardScreen, the home
                 screen (see docs/architecture/dashboard.md), and
                 WifiSetupScreen, the Touch UI fallback for initial
                 Wi-Fi provisioning (see networking.md) - both reused
                 directly by both the simulator and firmware. Also
                 AppCore - the composition root every one of the above
                 (plus most of core/, see above) is built and wired
                 through, shared by firmware/main/homedeck.cpp's
                 app_main() and simulator/main.cpp's main() so the same
                 ~20-object graph isn't hand-duplicated across both entry
                 points. Two-phase on purpose (construct, then an
                 explicit Start()) so EventBus subscription order no
                 longer depends on construction order - see its own
                 header comment.
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
`firmware/main/CMakeLists.txt`'s own comment for why).

`core/module.h` is the module lifecycle contract ADR-0003 deferred until
Harmony (the reference module) needed it for real - `Start()`/`Stop()`,
construction as Init, the destructor as teardown. `core/harmony_connection.h`/
`.cpp` is the first (and so far only) implementation: hub connection over
a new `platform/websocket_client.h` (`HostWebSocketClient`/
`FirmwareWebSocketClient`, see
[ADR-0029](../docs/decisions/ADR-0029-harmony-local-protocol.md)), a
generic `core/retry_backoff.h` exponential-backoff utility
([ADR-0006](../docs/decisions/ADR-0006-networking-discovery-provisioning.md#decision-retrybackoff-policy-ownership)'s
previously-unbuilt default), and `core/harmony_routes.h`/`.cpp` for its
Web UI status/reconnect endpoints. Screens, dashboard widgets, and
command-sending are still ahead - see
[roadmap.md](../docs/roadmap.md)'s M3 section.
