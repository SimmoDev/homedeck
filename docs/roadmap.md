# Roadmap

This roadmap tracks the milestones defined in [CLAUDE.md](../CLAUDE.md). Milestones are
sequential — scope is not expanded until the current milestone's goal is
complete, per the project's scope-control philosophy.

## M0 — Foundation (complete)

**Goal:** establish the architectural and documentation foundation before
any implementation begins.

- [x] Repository structure
- [x] Architecture documentation
- [x] Architectural Decision Records (ADR-0001–0013)
- [x] README and DEVELOPMENT guide
- [x] Confirm open decisions flagged across the ADRs and architecture docs
      (see [Architectural Decisions Index](#architectural-decisions-index)
      below) — all resolved except the Harmony local-control investigation
      (in progress, scoped) and the module interface (intentionally
      deferred to M3 by design)
- [x] Development environment set up and verified — the
      `espressif/idf:v5.4.3` Docker image (`esp32p4` target support, a
      real `idf.py build` producing a flashable `.bin` — see
      [hardware.md](architecture/hardware.md#display-and-touch) for why
      this specific version is pinned), and simulator build prerequisites
      (CMake, Ninja, SDL2, C++20 — see the [simulator
      scaffold](../simulator/README.md)). See
      [DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup).

**Exit criteria met:** a developer can follow
[DEVELOPMENT.md](../DEVELOPMENT.md) to get a working build environment for
both targets.

## M1 — Platform (complete)

A Tab5 boots into the real dashboard (live clock, real battery reading),
and the same UI runs in the desktop simulator. The ESP32-C6 power/SDIO
domain question resolved into two parts: wiring independence, confirmed
here; a separate protocol-level question (whether ESP-Hosted/SDIO can
stay associated while the P4 sleeps), tracked under M2's "Power
management state model" item instead.

- [x] Simulator scaffold — a host-native CMake project with LVGL's SDL2
      driver, LVGL pinned to `v9.5.0` via `FetchContent` (see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#decision-build-system)
      and [simulator/README.md](../simulator/README.md)).
- [x] CI and unit test framework — three independent GitHub Actions
      workflows, one per job (separate files, each with its own status
      badge — see [README.md](../README.md)): `simulator`, `tests`
      (GoogleTest+GoogleMock, its own host-native CMake project per
      [tests/README.md](../tests/README.md) — see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#5-test-framework)),
      and `firmware` (the `espressif/idf:v5.4.3` Docker image). See
      [DEVELOPMENT.md](../DEVELOPMENT.md#continuous-integration).
- [x] Reference hardware confirmed — the K145 kit (see
      [hardware.md](architecture/hardware.md#power)) with the **ST7123**
      integrated display+touch driver, detected at runtime per
      [ADR-0009](decisions/ADR-0009-touch-display-detection.md).
- [x] ESP-IDF project scaffolding — `idf.py set-target esp32p4 build`
      produces a real `homedeck.bin` (see
      [firmware/README.md](../firmware/README.md)).
- [x] Tab5 boot — confirmed on real hardware over USB (see
      [DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup) for the
      flash/monitor procedure, including manual download-mode entry).
- [x] ESP32-C6 co-processor power/SDIO domain wiring — **confirmed
      independent** of the P4's deep-sleep domain: the C6's power rail is
      independently switchable (I2C GPIO expander, no hardware coupling
      to P4 sleep state — see
      [hardware.md](architecture/hardware.md#wireless)). Whether
      ESP-Hosted/SDIO can actually keep the C6 usefully associated while
      the P4 itself is asleep turned out to be moot: the P4 never enters
      deep sleep in this project's design at all — see
      [ADR-0024](decisions/ADR-0024-sleeping-wake-mechanism.md).
- [x] Display and touch bring-up — real pixels and working touch input
      confirmed on the Tab5 panel via `espressif/m5stack_tab5`, not
      M5GFX/M5Unified (see
      [hardware.md](architecture/hardware.md#display-driver-strategy)).
      Panel orientation resolved as portrait, no rotation — see
      [ADR-0015](decisions/ADR-0015-display-orientation.md).
- [x] Basic LVGL application running **on-device** — the real dashboard
      (`EventBus`, `Clock`, `DashboardScreen`, reused directly from
      `src/`, not reimplemented) runs live on the Tab5, with real sensor
      data: a live ticking clock and a real (not mocked) battery
      percentage. Built on `src/platform/firmware/` — FreeRTOS-backed
      `Task`/`Timer` (per ADR-0002), `BatteryReader` via the INA226
      (`espp/ina226`), and `TimeSource` via the RX8130CE RTC
      (`espp/rx8130ce`) — see
      [hardware.md](architecture/hardware.md#on-device-dashboard).
      `Queue<T>`'s firmware backend stays deferred — nothing uses it yet.
      Navigation, the home affordance, and a second screen are out of
      scope for this item specifically.
- [x] Desktop simulator target running the same application — a separate
      host-native CMake project, Core Concurrency Abstraction backed by
      the C++ standard library (see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#decision-build-system)).
      `Task`/`Queue`/`Timer`/`EventBus` all have real unit tests in
      [tests/](../tests/). Portable source lives in
      [src/](../src/) — see [src/README.md](../src/README.md) for the
      layout.
- [x] Initial dashboard shell — `DashboardScreen` (see
      [src/README.md](../src/README.md)). Core-only widgets, hardcoded
      directly — the pluggable widget-registration system and grid
      layout are M2 scope (see
      [dashboard.md](architecture/dashboard.md#status) and
      [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-dashboard-layout-model)).
- [x] Persistent home affordance included in the base screen layout from
      the first non-dashboard screen onward (see
      [ADR-0004](decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance)).
      A minimal real Navigation manager (`src/ui/navigation.h` -
      `Register`/`GoTo`/`GoHome`) and a reusable `LV_SYMBOL_HOME`
      affordance (`src/ui/home_affordance.h`), proven at the time against
      a deliberately throwaway second screen — replaced once a genuine
      one existed, M2's Wi-Fi setup screen below (see
      [ui.md](architecture/ui.md#status)).
- [x] Clock/date display — `Clock` (`src/core/`) publishes a
      `ClockTickEvent` once a second via the `EventBus`, plus once
      immediately at construction so the display never shows LVGL's
      placeholder text before the first periodic tick (see
      `tests/clock_test.cpp`). `TimeSource`'s host backend wraps
      `std::chrono::system_clock`.
- [x] Battery status display — the simulator's `HostBatteryReader`
      returns a mock value; real hardware reads the actual INA226 (see
      the on-device dashboard item above).

**Exit criteria:** a Tab5 boots into a minimal but real HomeDeck UI showing
live clock and battery status, and the same UI runs in the desktop
simulator.

## M2 — Platform Services (current)

- [x] Wi-Fi connectivity, including initial provisioning (SoftAP + a
      minimal HTTP setup form, Touch UI keyboard entry as a fallback via
      `WifiSetupScreen` — see
      [networking.md](architecture/networking.md#initial-wi-fi-provisioning)
      and [ADR-0026](decisions/ADR-0026-wifi-provisioning-mechanism.md)
      for the provisioning mechanism). Implemented and confirmed on
      hardware, including with a non-alphanumeric SSID — SoftAP
      provisioning, the Touch UI fallback, and reconnecting to stored
      credentials, all via `firmware/main/wifi_setup.cpp`. The setup
      form's submitted SSID/password are percent-decoded and
      length-validated (`src/core/url_codec.h`,
      `src/core/wifi_credentials.h`, both host-tested) before being
      applied to `esp_wifi`. See
      [hardware.md](architecture/hardware.md#wi-fi-bring-up) for a
      separate, intermittent, not-yet-root-caused crash risk during the
      Wi-Fi-connect burst, unrelated to this parsing/validation logic —
      the Web UI's Diagnostics page now has a **Reset Wi-Fi credentials**
      action (`POST /api/wifi/reset`, see
      [web-ui.md](architecture/web-ui.md#diagnostics)) purpose-built to
      reproduce that crash repeatedly without a full
      `tools/factory-reset.sh` cycle per attempt — a diagnostic aid, not
      the M7 "Web Management UI factory-reset option" below, whose own
      scope is still undecided.
      Still open: wiring credential
      storage into Core's Configuration/Storage service instead of
      `esp_wifi`'s own default persistence on the C6 co-processor — a
      documented, low-priority gap (see
      [hardware.md](architecture/hardware.md#wi-fi-bring-up)), not active
      scope, since both locations are equally unencrypted flash and no
      feature depends on it.
- [x] LAN discovery (thin mDNS wrapper — see
      [networking.md](architecture/networking.md#lan-discovery)).
      Self-advertisement is implemented and confirmed on hardware — the
      device advertises as `homedeck.local` once Wi-Fi connects (ESP-IDF's
      `mdns` component, called directly from `firmware/main/homedeck.cpp`).
      The mDNS *browsing* wrapper for modules to discover Home
      Assistant/Kodi is out of scope here — tracked against M4/M6
      instead, once one of those modules is a real consumer.
- [x] RTC time synchronization — the RX8130CE RTC (see
      [hardware.md](architecture/hardware.md#rtc)) had never been set,
      a real gap this item closes now that Wi-Fi exists to fix it over.
      SNTP against `pool.ntp.org` once Wi-Fi connects, corrected back
      into the physical RTC on every sync (`Rx8130TimeSource::SetTime()`,
      `firmware/main/homedeck.cpp`'s `StartTimeSync()`) rather than a
      manual set-time affordance — see
      [ADR-0028](decisions/ADR-0028-time-synchronization.md) for why.
      Firmware-only, same as crash/reboot diagnostics — the simulator's
      host OS already keeps correct time on its own. Timezone support
      (the RTC's fields are still interpreted with no TZ conversion
      anywhere in this project) remains a separate, real gap, not solved
      here — a future item, not yet placed against a specific milestone.
- [x] Configuration service (storage-backed) across the storage tiers
      (NVS, internal flash filesystem — see
      [ADR-0012](decisions/ADR-0012-storage-tiers.md)), with a schema
      version field on every persisted blob (see
      [ADR-0010](decisions/ADR-0010-secret-storage.md)) and per-module
      namespacing enforced by the service itself, not by convention.
      `Storage` (`src/core/storage.h`) is implemented for the NVS and
      internal-flash-FAT tiers, unit-tested in `tests/` and confirmed on
      hardware; `SecretStore` is implemented alongside it
      (`AdminAuthService`'s password hash uses it). NVS encryption is
      not M2 scope (see
      [ADR-0018](decisions/ADR-0018-staged-security-hardening.md)); the
      optional microSD tier is a separate M7 item below.
- [x] Web Management UI (settings, diagnostics,
      backups as a downloadable JSON export —
      *not* initial Wi-Fi setup, which is the SoftAP flow above; see
      [web-ui.md](architecture/web-ui.md#relationship-to-wi-fi-provisioning)),
      including the first-login admin password setup screen and API
      input validation on every endpoint (see
      [security.md](architecture/security.md#requirement-validate-api-input)).
      Auth, static assets, the first-login/session UI, diagnostics, OTA,
      settings, backups, and weather-location search are all implemented
      and confirmed on both the simulator and hardware — see
      [web-ui.md](architecture/web-ui.md#status) for the full detail.
      Module configuration is tracked as its own M3 item below (no module
      exists yet to configure); WebSockets, Wi-Fi management, and
      a factory-reset option are tracked as their own M7 items below.
- [x] OTA update support, gated on battery threshold or external USB-C
      power (see
      [power-management.md](architecture/power-management.md#explicit-power-states)).
      Implemented and confirmed on hardware — `GET /api/ota/status`,
      `POST /api/ota/upload`, `POST /api/ota/reboot`
      (`src/core/ota_routes.h`/`.cpp`), gated by `EvaluateOtaGate()`
      (`src/core/ota_gate.h`) per
      [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate),
      admin-only via `RequireAuth()`, with app-rollback
      (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) confirmed reverting to
      the previous slot after a bad image. Image signing remains a
      known, deliberately deferred gap (see
      [security.md](architecture/security.md#ota-image-integrity)).
      Simulated on the simulator (upload/progress/gating against mocked
      battery, no real partition writes — see
      [simulator.md](architecture/simulator.md#how-it-works)).
- [x] Logging, including reset-reason tracking and core dump capture on
      panic (dedicated flash partition, downloadable raw via Web UI, not
      symbolicated on-device — see
      [ADR-0013](decisions/ADR-0013-crash-and-reboot-diagnostics.md)).
      Crash/reboot diagnostics, their Web UI presentation, and the
      general structured/leveled logging facility (`Logger`,
      `src/core/logger.h`/`.cpp`, `GET /api/diagnostics/logs` — see
      [ADR-0019](decisions/ADR-0019-structured-logging.md)) are all
      implemented and confirmed on hardware, including a real triggered
      panic and clean reboot — see
      [diagnostics.md#status](architecture/diagnostics.md#status) for the
      full detail. Persistence is asynchronous and batched (see
      [ADR-0020](decisions/ADR-0020-async-log-persistence.md) and
      [ADR-0021](decisions/ADR-0021-xip-from-psram.md) for the display-
      glitch root cause this resolves). Extended log archival to microSD
      is tracked as its own M7 item below.
- [x] Audio bring-up (ES8388 codec, 1W speaker output — see
      [hardware.md](architecture/hardware.md#audio) for the confirmed
      pins/I2C/feature-enable facts). A platform capability in its own
      right, not Notifications-specific — Notifications' sound
      presentation (below) depends on this rather than duplicating it.
      `AudioOutput` (`src/platform/`) is the portable interface —
      mono 16-bit PCM, blocking, paced in real time on both targets —
      with `FirmwareAudioOutput` (`esp_codec_dev`) and `HostAudioOutput`
      (SDL2) implementations, confirmed audible on hardware. On-device
      volume control is covered by the Power management item below.
      Still open: the ES7210 mic input, not wired up - no capability
      needs it yet.
- [x] Notifications service, with presentation (banners, sound, dashboard
      indicators) mapped to existing mechanisms rather than designed fresh
      (see [ui.md](architecture/ui.md#notification-presentation)) —
      vibration is out of scope, no motor exists on the confirmed BOM.
      All three presentation outputs are implemented: `NotificationEvent`/
      `NotificationSeverity` (`src/core/notification.h`, carrying the
      alert-priority/deferred urgency
      [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping)
      requires) published via `EventBus`, with `LowBatteryMonitor`
      (`src/core/`) as the first real publisher (latched so a sustained
      low-battery state notifies once, not once a second). The three
      outputs — `NotificationBanner`, `NotificationSound`, and
      `NotificationWidget` (all `src/ui/`) — are confirmed together on
      both the simulator and hardware. The alert-priority/deferred
      urgency distinction this service carries no longer gates any
      wake-cycle behavior (see
      [ADR-0024](decisions/ADR-0024-sleeping-wake-mechanism.md)) - it
      remains available for a future presentation difference, not a gap
      in this pass. `PostToUiThread` (`src/ui/ui_dispatch.h`/`.cpp`)
      queues UI-bound updates through its own `Queue<T>` and drains it
      via a single recurring timer, guaranteeing FIFO delivery order
      independent of `lv_async_call`'s own ordering - see
      [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md)'s Consequences
      for detail. Distinct sounds per severity (deferred until a second
      severity has a real publisher) and an unread-count badge (as an
      alternative to `NotificationWidget`'s last-notification-tile
      design) are separate M7 items below.
- [x] Widget framework (general dashboard widget interface), including
      the weather widget (Core `WeatherProvider` interface, Open-Meteo
      as the direct provider — see
      [dashboard.md](architecture/dashboard.md#weather-source)).
      Implemented and confirmed on hardware — see
      [dashboard.md](architecture/dashboard.md#status) for the
      `Widget`/`DashboardGrid` interface and every widget built on it
      (`ClockWidget`, `NetworkStatusWidget`, `WeatherWidget`,
      `NotificationWidget`). Still open, deliberately out of scope for
      this pass: weather condition icons and Fahrenheit/Celsius
      selection (both M7 polish, see the M7 section below); a tap
      handler on `Widget` itself (no widget has one today) - deferred
      until Harmony (M3) or Kodi (M4) first need tap-for-detail, rather
      than designed speculatively against weather alone; and the
      enable/disable/reorder widget customization named in
      [dashboard.md](architecture/dashboard.md#customization-future),
      separate M7 scope.
- [x] Status bar (persistent date/time and battery, shown on every screen —
      not a dashboard widget, see
      [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets)).
      Implemented and confirmed on hardware — `StatusBar`
      (`src/ui/status_bar.h`/`.cpp`), constructed by every screen (see
      [dashboard.md](architecture/dashboard.md#status)), including
      charging/no-battery detection backed by real INA226/IO-expander
      signals (see [hardware.md](architecture/hardware.md#power)) and a
      Wi-Fi connectivity icon backed by the portable `NetworkStatus`/
      `NetworkStatusMonitor` abstraction (see
      [networking.md](architecture/networking.md#status)). Still open:
      the Web UI's Wi-Fi management page, a separate, now-unblocked
      follow-up; and `clock_label_` showing blank for up to one Clock
      period (~1s) after construction rather than the correct time
      immediately, a minor, non-jarring gap deferred until `TimeSource`
      is threaded through `StatusBar`'s constructor.
- [x] Power management state model — `PowerManager`
      (`src/core/power_manager.h`/`.cpp`) is implemented for
      `Active`/`Idle`/`Sleeping`/`Updating`/`Error`, including the
      sleep-veto mechanism (unit-tested; `RequestSleepVeto` itself still
      has no module caller until M3+), and confirmed on hardware
      including wake-to-full-brightness from `Sleeping` on real touch.
      `Sleeping` isn't real ESP32 deep sleep - display off, CPU stays
      active, touch-wake by polling - since M5Stack's own official Tab5
      firmware uses the same pattern for the same reason (see
      [ADR-0024](decisions/ADR-0024-sleeping-wake-mechanism.md)).
      `Error` is implemented for one of its three ADR-0005-scoped fault
      types: `CriticalBatteryMonitor`
      (`src/core/critical_battery_monitor.h`/`.cpp`) transitions
      `PowerManager` into `kError` once the battery crosses a critical
      threshold while not on external power, taking priority over
      `kUpdating` if both are true at once. Charging-fault and
      thermal-fault detection are permanent limitations of this board
      revision, not deferred scope — see
      [power-management.md](architecture/power-management.md#status) and
      [hardware.md](architecture/hardware.md#power) for why neither
      signal exists on this hardware. On-device manual brightness and
      volume control are implemented through one shared
      `QuickSettingsPanel` (`src/ui/quick_settings_panel.h`/`.cpp`),
      applying live on every slider drag and persisting to `Storage` on
      release, confirmed surviving a real power cycle. Sliders show a
      plain text label for both controls, not an icon (see the M7
      polish item for why).
- [x] Simulator physical-keyboard input (dev tooling, not product scope) —
      `lv_sdl_keyboard_create()` plus a default `lv_group` for focus/Tab
      routing (`UiTask`, `src/platform/host/ui_task.cpp`), so typing into a text
      field (e.g. `WifiSetupScreen`) works directly from a physical
      keyboard instead of only by clicking the on-screen one. Confirmed
      manually in the simulator — see
      [simulator.md](architecture/simulator.md#status).

**Exit criteria:** the device can be provisioned onto Wi-Fi via SoftAP,
administered over the Web UI (after setting an admin password on first
login) once on the LAN, kept updated over OTA, and the dashboard/widget
framework is ready for a real module to plug into.

## M3 — Harmony

**Goal:** a complete Harmony Hub replacement. Scope does not expand beyond
this until it's done — see
[ADR-0001](decisions/ADR-0001-project-vision.md).

- [ ] Hub discovery on the LAN (scoped to already-paired hubs — see [known
      risk in
      ADR-0003](decisions/ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control))
- [ ] Local authentication against an already-paired hub (local XMPP-based
      protocol). First-time cloud pairing is out of scope unless it later
      proves necessary.
- [ ] Activities (list, start, current, status)
- [ ] Devices (enumerate, capabilities, commands, inputs, power state where
      available)
- [ ] Remote control (navigation, volume, channel, numeric keypad, transport
      controls, long-press actions where supported)
- [ ] Status/events integrated with Core's event bus and notifications
- [ ] Web Management UI module configuration page for Harmony (hub
      IP/credentials) — the generic settings API (M2) is ready for a
      consumer; this is its first real one

**Exit criteria:** a user can fully replace their physical Harmony remote's
day-to-day usage with HomeDeck.

## M4 — Media

- [ ] Kodi integration — needs the mDNS *browsing* wrapper deferred from
      M2's LAN discovery item (see
      [networking.md](architecture/networking.md#lan-discovery)); this is
      its first real consumer
- [ ] Media browsing
- [ ] Playback control
- [ ] Now Playing widget/screen

## M5 — Monitoring

- [ ] Uptime Kuma integration
- [ ] Service status dashboard
- [ ] Notifications integration - publishing `NotificationEvent`s from a
      monitor going down, through the module's normal background task
      (no special Sleeping-specific hook needed, see
      [ADR-0024](decisions/ADR-0024-sleeping-wake-mechanism.md))

## M6 — Home Automation

- [ ] Home Assistant integration — also a consumer of the mDNS
      *browsing* wrapper deferred from M2's LAN discovery item (see
      [networking.md](architecture/networking.md#lan-discovery)), if
      discovery rather than manual configuration is used
- [ ] Devices
- [ ] Scenes
- [ ] Dashboards
- [ ] HA-sourced `WeatherProvider`, offered as an alternative to the direct
      Open-Meteo provider from M2 (see
      [dashboard.md](architecture/dashboard.md#weather-source))

## M7 — Polish

- [ ] Themes
- [ ] Animations
- [ ] Accessibility
- [ ] Performance optimisation
- [ ] Battery optimisation
- [ ] Full board power-off with RTC-alarm cold-boot wake, matching
      M5Stack's own official Tab5 firmware pattern
      (`sleepAndRtcWakeup()`/`powerOff()` via the PMS150G-U06 power
      controller) - a real, standalone feature, deliberately decoupled
      from `Sleeping` rather than something it depends on, since it
      needs its own from-scratch hardware driver (the PMS150G's
      power-off pulse timing isn't in any official spec, only
      reverse-engineered by the community so far) - see
      [ADR-0024](decisions/ADR-0024-sleeping-wake-mechanism.md)
- [ ] User customisation (dashboard widget/layout customization — see
      [dashboard.md](architecture/dashboard.md#customization-future))
- [ ] Standard-tier security hardening: activate NVS encryption via the
      HMAC-peripheral scheme, including its one-time eFuse provisioning
      step — see [ADR-0018](decisions/ADR-0018-staged-security-hardening.md).
      Timing within M7 is a placeholder, not fixed: the actual trigger is
      a real module credential existing to protect (see ADR-0018), which
      may land earlier once M3+ modules are built
- [ ] Weather condition icons for `WeatherWidget` (`src/ui/weather_widget.cpp`),
      replacing the current text-only WMO condition mapping - custom
      icon assets, out of scope for the widget's first pass (see
      [dashboard.md](architecture/dashboard.md#status))
- [ ] Celsius/Fahrenheit unit selection for `WeatherWidget` - currently
      hardcoded to Celsius (`OpenMeteoWeatherProvider`,
      `src/core/weather_provider.cpp`)
- [ ] Richer weather detail (hourly/daily forecast) reachable by tapping
      the weather tile, once `Widget` gains a tap handler (see the
      Widget framework item's own note in M2)
- [ ] Web Management UI dark theme - no `prefers-color-scheme` support
      anywhere in `webui/src`; `index.html` and every component's own
      scoped `<style>` block hardcode light-theme literal colors with
      no shared CSS custom properties, so adding this later means
      touching every component individually. Distinct from the
      "Themes" item above, which is the on-device Touch UI (LVGL)
- [ ] WebSockets for live Web Management UI updates - needs confirming
      civetweb's safe cross-task dispatch mechanism first
      (`esp_http_server`'s side is `httpd_queue_work()`, civetweb's
      equivalent is not yet confirmed — see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server))
- [ ] Web Management UI Wi-Fi management (view/change credentials
      post-provisioning) - currently only possible via
      `tools/factory-reset.sh` + reprovisioning
- [ ] Web Management UI factory-reset option (clearing stored Wi-Fi
      credentials, per
      [hardware.md](architecture/hardware.md#wi-fi-bring-up) for where
      those actually live, plus Core's own `Storage` state) - scope (a
      dedicated action vs. part of a broader reset, what exactly gets
      cleared) isn't decided yet
- [ ] Extended log archival to microSD, once a card is present - the
      internal flash filesystem's logs (M2) are bounded/rotating;
      [ADR-0012](decisions/ADR-0012-storage-tiers.md) decides this is
      the microSD tier's purpose, degrading gracefully (clearly
      indicating "no card present") when none is inserted, per
      [CLAUDE.md](../CLAUDE.md)'s "work fully using stock Tab5 hardware" requirement
- [ ] Gesture navigation (Android-style): swipe up from the bottom edge
      to go home, swipe in from the left/right edge to go back. The
      home-swipe would be an addition alongside the persistent home
      icon, not a replacement - see
      [ADR-0004](decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance)
      for why an edge gesture was rejected as the *guaranteed*
      mechanism (risk of conflicting with a future module screen's own
      gesture use, e.g. a media carousel or a scrollable list near the
      edge) but left open as a later power-user addition once modules
      exist and their real gesture needs are known. The back-swipe half
      needs a real prerequisite first: `Navigation`
      (`src/ui/navigation.h`) has no history/back-stack today, only
      `GoTo(route)`/`GoHome()`
- [ ] Swipe-down status-bar gesture (Android-style) to open the
      brightness/volume quick-settings panel (see the M2 Power
      management item), as a power-user addition alongside the panel's
      primary tap-the-icon affordance, not a replacement - same
      reasoning as the Gesture navigation item above
      ([ADR-0004](decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance)):
      an explicit tap target is the guaranteed mechanism, a gesture is
      an optional layered-on shortcut added once the tap-based version
      exists and its real usage is understood. Needs the panel itself
      built first
- [ ] Icon pair for the brightness/volume quick-settings panel's sliders
      (`src/ui/quick_settings_panel.cpp`), replacing the current
      text-label-above-slider styling for both - LVGL's bundled symbol
      font has a matching min/max pair for volume
      (`LV_SYMBOL_MUTE`/`LV_SYMBOL_VOLUME_MAX`) but no "brightness" glyph
      at all, so volume alone getting icons would look inconsistent
      against brightness; same "custom icon assets, out of scope for the
      first pass" reasoning as the Weather condition icons item below,
      not solved twice
- [ ] Distinct notification sounds per `NotificationSeverity`
      (`src/core/notification_sound.cpp`) - currently one tone for both
      `kDeferred`/`kAlertPriority`, deferred until a second severity
      actually has a real publisher (see the M2 Notifications item)
- [ ] Nicer notification sound design generally - `NotificationSound`'s
      current tone is a plain generated sine wave (no audio asset
      pipeline exists yet - see the M2 Audio bring-up item), functional
      but not a considered piece of sound design
- [ ] A startup sound played when the splash screen displays
      (`ShowSplashScreen()`, `firmware/main/homedeck.cpp`) - not
      designed yet, and not necessarily `NotificationSound` itself
      (boot isn't a notification), but the same `AudioOutput` interface
      it's built on
- [ ] Unread-notification-count badge as an alternative/addition to
      `NotificationWidget`'s current last-notification-tile design (see
      the M2 Notifications item) - needs a new Core-owned counter plus a
      mark-as-seen rule (on dashboard view? on tap?) that doesn't exist
      yet and has no natural trigger point today, deliberately deferred
      rather than built speculatively ahead of a second notification
      publisher actually existing

## Architectural Decisions Index

M0's job was resolving the open questions this list used to itemize in
full; that reasoning now lives in the ADRs themselves
([docs/decisions/](decisions/)), not duplicated here. This is just an
index — decision name, ADR, one-line outcome.

| Decision | ADR | Outcome |
|---|---|---|
| Project license | [ADR-0001](decisions/ADR-0001-project-vision.md#decision-license) | MIT |
| Firmware stack, build system | [ADR-0002](decisions/ADR-0002-technology-stack.md) | ESP-IDF/C++/FreeRTOS/LVGL; `idf.py` (firmware) + separate host-native CMake (simulator) |
| Core concurrency abstraction | [ADR-0002](decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction) | `Task`/`Queue`/`Timer` interface — FreeRTOS-backed (firmware) / C++ stdlib-backed (simulator) |
| Simulator rendering backend | [ADR-0002](decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend) | LVGL SDL2 desktop backend |
| JSON library | [ADR-0002](decisions/ADR-0002-technology-stack.md#2-json-library) | nlohmann::json |
| Embedded HTTP/WebSocket server | [ADR-0002](decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server) | `esp_http_server` (firmware) + civetweb (simulator) |
| WebSocket relay dispatch safety | [ADR-0002](decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server) | `httpd_queue_work()` on firmware; civetweb's equivalent **unconfirmed** — deferred to the M7 WebSockets item below |
| Web UI frontend | [ADR-0002](decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach) | Svelte 5 + TypeScript + Vite, plain client-side (no SvelteKit) |
| Web UI static asset storage | [ADR-0025](decisions/ADR-0025-webui-static-asset-storage.md) | Embedded in the firmware app image (`EMBED_FILES`), not the `storage` partition — avoids partition-wipe-on-reflash and frontend/backend OTA drift |
| Test framework | [ADR-0002](decisions/ADR-0002-technology-stack.md#5-test-framework) | GoogleTest/GoogleMock against the simulator; no on-target suite |
| Module architecture, Core/module boundary | [ADR-0003](decisions/ADR-0003-module-architecture.md) | Event bus only; Core stays generic; Harmony is the reference module |
| UI philosophy, state-management pattern | [ADR-0004](decisions/ADR-0004-ui-philosophy.md) | Touch UI vs. Web UI split; dashboard-as-home; lightweight per-screen controllers |
| Return-home affordance | [ADR-0004](decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance) | Persistent on-screen home icon, not a gesture or power-button long-press |
| Sleep-veto mechanism | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-sleep-veto-mechanism) | Event-based, time-limited |
| Alert-priority wake cycle | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping) | Periodic RTC wake (~2-5 min), alert-priority notifications only — superseded, see ADR-0024 |
| Power state 'Error' scope | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-error-state-scope) | Narrowed to power-specific faults |
| OTA battery/power gate | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate) | Battery threshold OR USB-C power required |
| Sleeping's wake mechanism | [ADR-0024](decisions/ADR-0024-sleeping-wake-mechanism.md) | No ESP32 deep sleep — CPU stays active, display off, touch-wake by polling, matching M5Stack's own official firmware |
| Retry/backoff ownership | [ADR-0006](decisions/ADR-0006-networking-discovery-provisioning.md#decision-retrybackoff-policy-ownership) | Shared Core utility by default |
| LAN discovery shape | [ADR-0006](decisions/ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape) | Thin Core mDNS wrapper only |
| Initial Wi-Fi provisioning | [ADR-0006](decisions/ADR-0006-networking-discovery-provisioning.md#decision-initial-wi-fi-provisioning-flow) | SoftAP + captive portal, Touch UI keyboard as fallback |
| Web UI authentication | [ADR-0007](decisions/ADR-0007-web-management-ui-policies.md#decision-authentication-mechanism) | Local admin password + session login |
| Admin password sequencing | [ADR-0007](decisions/ADR-0007-web-management-ui-policies.md#decision-when-the-admin-password-is-set) | First-login sets password, not part of SoftAP flow |
| Dashboard layout model | [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-dashboard-layout-model) | Fixed grid |
| Weather data source | [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-weather-data-source) | Pluggable `WeatherProvider`; Open-Meteo from M2, HA-sourced from M6 |
| Status bar vs. dashboard widgets | [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets) | Persistent top status bar (date/time, battery), not dashboard-grid widgets |
| Touch/display controller detection | [ADR-0009](decisions/ADR-0009-touch-display-detection.md) | Runtime detection via `espressif/m5stack_tab5`'s built-in probing — ADR-0009's own persisted-result/manual-override design is superseded, see ADR-0014 |
| Hardware support library (display/touch) | [ADR-0014](decisions/ADR-0014-hardware-support-library.md) | `espressif/m5stack_tab5` (ESP-IDF-native), not M5Unified/M5GFX — confirmed crash on this chip via Arduino-as-Component |
| Display orientation | [ADR-0015](decisions/ADR-0015-display-orientation.md) | Portrait, `720x1280`, no rotation — the panel's native scan direction; matches the battery pack's kickstand tilt |
| Hardware support library (battery/RTC) | [ADR-0016](decisions/ADR-0016-battery-rtc-library.md) | `espp/ina226` + `espp/rx8130ce` — the BSP's own capability table doesn't cover either peripheral |
| Secret storage | [ADR-0010](decisions/ADR-0010-secret-storage.md) | HMAC-peripheral NVS encryption scheme (confirmed independent of flash encryption on ESP32-P4) chosen but not yet active + hashed admin password (active now) + `SecretStore` interface (implemented, backs the admin password hash) |
| Staged security hardening | [ADR-0018](decisions/ADR-0018-staged-security-hardening.md) | Development (now) → Standard (NVS encryption, once a real module credential exists) → Hardened (Secure Boot + flash encryption, only if HomeDeck is ever manufactured) |
| OTA image signing | [security.md](architecture/security.md#ota-image-integrity) | Known gap, deliberately deferred — not yet in scope |
| LVGL thread safety | [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md) | Dedicated UI task owns LVGL; event bus guarantees safe hand-off via `lv_async_call()` |
| Event payload lifetime | [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md#decision-event-payload-lifetime-across-the-dispatch-boundary) | Reference-counted copy at publish time, not a raw pointer into publisher state |
| Storage tiers | [ADR-0012](decisions/ADR-0012-storage-tiers.md) | NVS (credentials) / internal flash (cache, logs) / optional microSD (extended log archival only — not backups) |
| Internal flash filesystem | [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-internal-flash-filesystem-choice) | FAT + `wear_levelling` (in-tree, not third-party LittleFS) |
| Storage namespacing | [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-storage-namespacing) | Enforced per-module by the storage service, not a naming convention |
| Backup delivery | [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-backup-delivery) | Always Web UI JSON download, regardless of SD presence |
| Crash/reboot diagnostics | [ADR-0013](decisions/ADR-0013-crash-and-reboot-diagnostics.md) | Reset-reason tracking + core dump to a dedicated partition, raw download only |
| Partition table | [ADR-0017](decisions/ADR-0017-partition-table.md) | Custom `ota_0`/`ota_1` A/B scheme + dedicated core dump partition, replacing the built-in single-app table |
| Structured logging storage | [ADR-0019](decisions/ADR-0019-structured-logging.md) | Reuse `CacheStore`'s existing internal-flash tier — no new platform code |
| Async log persistence | [ADR-0020](decisions/ADR-0020-async-log-persistence.md) | `Logger::Log()` never blocks on flash I/O — a background `Task`/`Queue<T>` owns all writes |
| XIP from PSRAM | [ADR-0021](decisions/ADR-0021-xip-from-psram.md) | `CONFIG_SPIRAM_XIP_FROM_PSRAM=y` — resolves a flash-write-stalls-display-DMA glitch |
| Panel init settle delay | [ADR-0022](decisions/ADR-0022-panel-init-settle-delay.md) | Added delays after Sleep Out/Display On in the ST7123 init table, fixing an intermittent grey/fade-in on boot |
| Settings/backup reserved-key guard | [ADR-0023](decisions/ADR-0023-settings-backup-api.md) | A reserved-key guard keeps the admin password hash out of the generic settings API — since superseded as the sole protection by ADR-0027's partition split |
| Wi-Fi provisioning mechanism | [ADR-0026](decisions/ADR-0026-wifi-provisioning-mechanism.md) | HomeDeck-owned SoftAP + HTTP form, not ESP-IDF's `wifi_provisioning` — no usable transport on this project's `esp_wifi_remote` stack |
| SecretStore partition separation | [ADR-0027](decisions/ADR-0027-secret-store-partition-separation.md) | A dedicated `secrets` NVS partition, not a shared one with SettingsStore — structurally excludes secrets from `ListAll()`/backups, not just the reserved-key guard |
| Time synchronization | [ADR-0028](decisions/ADR-0028-time-synchronization.md) | SNTP against `pool.ntp.org` once Wi-Fi connects, corrected back into the physical RTC on every sync — not a manual set-time affordance; no timezone handling added |
| Harmony local control feasibility | [ADR-0003](decisions/ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control) | Scoped to already-paired hubs; protocol specifics investigated in M3 |
| Module interface (exact API) | [modules.md](architecture/modules.md#status) | Deferred by design — defined when Harmony (M3) is built |
