# Roadmap

This roadmap tracks the milestones defined in CLAUDE.md. Milestones are
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
- [x] Development environment set up and verified — ESP-IDF v5.4.2 via the
      `espressif/idf` Docker image (confirmed `esp32p4` target support with
      a real `idf.py build` producing a flashable `.bin`), and simulator
      build prerequisites (CMake, Ninja, SDL2, C++20 — confirmed by
      actually building and running the [simulator
      scaffold](../simulator/README.md)). See [DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup).

**Exit criteria met:** a developer can follow
[DEVELOPMENT.md](../DEVELOPMENT.md) to get a working build environment for
both targets.

## M1 — Platform (current)

Every remaining unchecked item below needs the physical Tab5 (SKU
confirmation, boot, display/touch bring-up, the on-device LVGL app) —
everything simulator-buildable without hardware is done.

- [x] **First action, before anything else in M1:** stand up the
      simulator's host-native CMake project with LVGL's SDL2 driver (see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#decision-build-system)
      and [simulator.md](architecture/simulator.md#status)). Confirmed
      working: builds via CMake/Ninja, LVGL pinned to `v9.5.0` via
      `FetchContent`, renders a real SDL2 window. Still just a scaffold —
      no Core/UI/module source yet, see
      [simulator/README.md](../simulator/README.md).
- [x] CI and unit test framework, set up early rather than retrofitted once
      more code exists to protect. Confirmed working (built and verified
      locally with `act` before ever pushing — see
      [DEVELOPMENT.md](../DEVELOPMENT.md#continuous-integration)):
      three independent GitHub Actions workflows, one per job (separate
      files rather than one shared workflow, so each gets its own status
      badge — see [README.md](../README.md)): `simulator` (builds the
      existing scaffold), `tests` (GoogleTest+GoogleMock, its own
      host-native CMake project per [tests/README.md](../tests/README.md),
      not nested inside `simulator/` as this bullet originally said — see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#5-test-framework)),
      and `firmware` (builds via the `espressif/idf:v5.4.2` Docker image —
      initially skipped cleanly until "ESP-IDF project scaffolding" below
      landed, now a real build). The tests workflow runs a real smoke
      test (one plain assertion, one GoogleMock-based test) proving the
      framework runs, not just declared — real Core/module tests arrive
      alongside the code they test, not before it exists.
- [ ] Confirm reference hardware SKU/kit — **done:** the reference unit is
      the K145 kit (see [hardware.md](architecture/hardware.md#power)) —
      and which touch/display controller revision it ships with (GT911 vs.
      ST7123 vs. ST7121 — still open, needs I2C probing once firmware
      boots — see
      [hardware.md](architecture/hardware.md#display-and-touch))
- [x] ESP-IDF project scaffolding — confirmed working: `idf.py set-target
      esp32p4 build` produces a real `homedeck.bin` (see
      [firmware/README.md](../firmware/README.md)).
- [x] Tab5 boot — confirmed on real hardware: flashed and booted cleanly
      over USB (`idf.py -p /dev/ttyACM0 flash monitor` via the Docker
      workflow, `--device` passthrough — see
      [DEVELOPMENT.md](../DEVELOPMENT.md#esp-idf-setup) for the confirmed
      procedure, including manual download-mode entry), heartbeat loop
      confirmed over the serial console. Fixed two sdkconfig gaps found
      against the real chip via `firmware/sdkconfig.defaults` (both
      expected, not hardware faults — see
      [hardware.md](architecture/hardware.md#application-processor)):
      flash size now correctly 16MB (was defaulting to 2MB), and the
      confirmed 32MB PSRAM now initializes correctly (was reporting 0
      free bytes with SPIRAM disabled by default).
- [ ] Confirm whether the ESP32-C6 co-processor's power/SDIO domain is
      independent of the P4's deep-sleep domain — determines whether the
      alert-priority wake cycle's cost model is full re-association or
      modem-sleep resume, not just its tuned interval (see
      [hardware.md](architecture/hardware.md#wireless) and
      [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping))
- [ ] Display bring-up, including confirming whether M5GFX/M5Unified's
      existing Tab5 support already handles touch/display controller
      detection before writing new detection code (see
      [ADR-0009](decisions/ADR-0009-touch-display-detection.md))
- [ ] Touch input bring-up, including runtime controller detection if not
      already handled by M5Unified (I2C probing + persisted result — see
      [ADR-0009](decisions/ADR-0009-touch-display-detection.md))
- [ ] Basic LVGL application running **on-device** — blocked on hardware.
      The mechanism itself (dedicated UI task owning LVGL exclusively,
      event payload types built on reference-counted copy from the first
      event type onward — see
      [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md)) is already
      built and running in the simulator (below); this item is porting
      the same `src/` code to real hardware, not designing it from
      scratch.
- [x] Desktop simulator target running the same application (separate
      host-native CMake project, Core Concurrency Abstraction backed by
      the C++ standard library — see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#decision-build-system)).
      Confirmed working end to end, not just compiling: a background
      `Timer` publishes a reference-counted `HeartbeatEvent` once a
      second, delivered safely to the dedicated UI task via `EventBus`'s
      `lv_async_call()` hand-off, and rendered on screen — verified by
      screenshot, the counter visibly incrementing. `Task`/`Queue`/
      `Timer`/`EventBus` all have real unit tests in
      [tests/](../tests/), not just the app exercising them. Portable
      source lives in the new [src/](../src/) directory (a repository
      structure question CLAUDE.md's own diagram left open — see
      [src/README.md](../src/README.md) for the layout and why).
- [x] Initial dashboard shell — `DashboardScreen` (see
      [src/README.md](../src/README.md)), replacing the throwaway
      heartbeat proof-of-mechanism screen. Confirmed working by
      screenshot, not just compiling. Core-only widgets, hardcoded
      directly (no pluggable widget-registration system, no grid layout)
      — both explicitly M2 scope per
      [dashboard.md](architecture/dashboard.md#status) and
      [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-dashboard-layout-model).
- [x] Persistent home affordance included in the base screen layout from
      the first non-dashboard screen onward (see
      [ADR-0004](decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance)).
      A minimal real Navigation manager (`src/ui/navigation.h` -
      `Register`/`GoTo`/`GoHome`, not a hardcoded two-screen switch) and
      a reusable `LV_SYMBOL_HOME` affordance
      (`src/ui/home_affordance.h`), proven against a deliberately
      throwaway second screen
      (`simulator/screens/placeholder_screen.h`, mirroring the earlier
      heartbeat screen's role) — confirmed working in both directions by
      manually running the simulator and tapping both buttons, not just
      compiling. Replaced once a genuine second screen exists (an M2
      settings screen, or the first M3 module screen).
- [x] Clock/date display — `Clock` (`src/core/`) publishes a
      `ClockTickEvent` once a second via the `EventBus`, plus once
      immediately at construction so the display never shows LVGL's
      placeholder text before the first periodic tick (a real bug caught
      by screenshot, fixed, and covered by a dedicated test — see
      `tests/clock_test.cpp`). `TimeSource`'s host backend wraps
      `std::chrono::system_clock`.
- [x] Battery status display — `BatteryReader` returns a fixed mock
      value. [simulator.md](architecture/simulator.md#how-it-works)
      documents battery as adjustable via a debug control; that control
      isn't built here — there's no real consumer for *adjustable* mock
      battery yet (power-management testing, M2 scope), just a widget
      that needs to render a number.

**Exit criteria:** a Tab5 boots into a minimal but real HomeDeck UI showing
live clock and battery status, and the same UI runs in the desktop
simulator.

## M2 — Platform Services

- [ ] Wi-Fi connectivity, including initial provisioning (SoftAP + captive
      portal via ESP-IDF's `wifi_provisioning` component, Touch UI keyboard
      entry as fallback — see
      [networking.md](architecture/networking.md#initial-wi-fi-provisioning))
- [ ] LAN discovery (thin mDNS wrapper — see
      [networking.md](architecture/networking.md#lan-discovery))
- [ ] Configuration service (storage-backed) across the three storage tiers
      (NVS encrypted using the HMAC-peripheral key scheme, internal flash
      filesystem, optional microSD — see
      [ADR-0012](decisions/ADR-0012-storage-tiers.md)), with a schema
      version field on every persisted blob (see
      [security.md](architecture/security.md#requirement-avoid-insecure-secret-storage)
      and [ADR-0010](decisions/ADR-0010-secret-storage.md)) and per-module
      namespacing enforced by the service itself, not by convention (see
      [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-storage-namespacing)) —
      manufacturing/first-flash must burn the HMAC eFuse key before this
      works on a given unit (see
      [ADR-0010](decisions/ADR-0010-secret-storage.md#consequences))
- [ ] Web Management UI (settings, module configuration, diagnostics,
      backups as a downloadable JSON export —
      *not* initial Wi-Fi setup, which is the SoftAP flow above; see
      [web-ui.md](architecture/web-ui.md#relationship-to-wi-fi-provisioning)),
      including the first-login admin password setup screen (hashed, not
      stored reversibly — see [web-ui.md](architecture/web-ui.md#admin-password))
      and API input validation on every endpoint (mechanism decided at
      implementation time — see
      [security.md](architecture/security.md#requirement-validate-api-input)),
      including confirming civetweb's safe cross-task dispatch mechanism
      for the WebSocket relay before building live updates on the
      simulator — `esp_http_server`'s side is `httpd_queue_work()`,
      civetweb's equivalent is not yet confirmed (see
      [ADR-0002](decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server))
- [ ] OTA update support, gated on battery threshold or external USB-C power
      (see [power-management.md](architecture/power-management.md#explicit-power-states)) —
      image signing is a known, deliberately deferred gap, not yet in scope
      (see [security.md](architecture/security.md#ota-image-integrity)) —
      simulated on the simulator (upload/progress/gating against mocked
      battery, no real partition writes) so the Web UI's OTA page doesn't
      need real hardware to build and test (see
      [simulator.md](architecture/simulator.md#how-it-works))
- [ ] Logging, including reset-reason tracking and core dump capture on
      panic (dedicated flash partition, downloadable raw via Web UI, not
      symbolicated on-device — see
      [ADR-0013](decisions/ADR-0013-crash-and-reboot-diagnostics.md)) —
      the core dump partition must be planned into the partition table
      alongside the existing OTA A/B scheme, not added later
- [ ] Notifications service, with presentation (banners, sound, dashboard
      indicators) mapped to existing mechanisms rather than designed fresh
      (see [ui.md](architecture/ui.md#notification-presentation)) —
      vibration is out of scope, no motor exists on the confirmed BOM
- [ ] Widget framework (general dashboard widget interface), including the
      weather widget (Core `WeatherProvider` interface, Open-Meteo as the
      direct provider — see [dashboard.md](architecture/dashboard.md#weather-source)) —
      first real case of a screen/widget being destroyed at runtime while
      subscribed to Core events, so resolve `EventBus`'s known
      cross-thread subscriber-lifetime gap (see [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md#consequences))
      before or alongside this, not after
- [ ] Status bar (persistent date/time and battery, shown on every screen —
      not a dashboard widget, see
      [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets)),
      replacing `DashboardScreen`'s current hardcoded clock/battery labels
- [ ] Power management state model, including the alert-priority wake-check
      cycle during Sleeping (interval tuned against real reconnect-cost/
      battery measurements on hardware — see
      [power-management.md](architecture/power-management.md#notifications-during-sleeping)),
      with the simulator's visual representation of power states (dimming,
      blackout, debug-triggered simulated wake sources) built alongside it,
      not as an afterthought — see
      [simulator.md](architecture/simulator.md#how-it-works)

**Exit criteria:** the device can be provisioned onto Wi-Fi via SoftAP,
administered over the Web UI (after setting an admin password on first
login) once on the LAN, keep itself updated over OTA, and the dashboard/
widget framework is ready for a real module to plug into.

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

**Exit criteria:** a user can fully replace their physical Harmony remote's
day-to-day usage with HomeDeck.

## M4 — Media

- [ ] Kodi integration
- [ ] Media browsing
- [ ] Playback control
- [ ] Now Playing widget/screen

## M5 — Monitoring

- [ ] Uptime Kuma integration
- [ ] Service status dashboard
- [ ] Notifications integration, including the alert-priority state-check
      hook used during the Sleeping wake cycle (see
      [power-management.md](architecture/power-management.md#notifications-during-sleeping))
      — this module is the reference implementation for that hook, same
      role Harmony plays for the general module contract

## M6 — Home Automation

- [ ] Home Assistant integration
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
- [ ] User customisation (dashboard widget/layout customization — see
      [dashboard.md](architecture/dashboard.md#customization-future))

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
| WebSocket relay dispatch safety | [ADR-0002](decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server) | `httpd_queue_work()` on firmware; civetweb's equivalent **unconfirmed**, needs M2 verification |
| Web UI frontend | [ADR-0002](decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach) | Svelte + Vite |
| Test framework | [ADR-0002](decisions/ADR-0002-technology-stack.md#5-test-framework) | GoogleTest/GoogleMock against the simulator; no on-target suite |
| Module architecture, Core/module boundary | [ADR-0003](decisions/ADR-0003-module-architecture.md) | Event bus only; Core stays generic; Harmony is the reference module |
| UI philosophy, state-management pattern | [ADR-0004](decisions/ADR-0004-ui-philosophy.md) | Touch UI vs. Web UI split; dashboard-as-home; lightweight per-screen controllers |
| Return-home affordance | [ADR-0004](decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance) | Persistent on-screen home icon, not a gesture or power-button long-press |
| Sleep-veto mechanism | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-sleep-veto-mechanism) | Event-based, time-limited |
| Alert-priority wake cycle | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping) | Periodic RTC wake (~2-5 min), alert-priority notifications only |
| Power state 'Error' scope | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-error-state-scope) | Narrowed to power-specific faults |
| OTA battery/power gate | [ADR-0005](decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate) | Battery threshold OR USB-C power required |
| Retry/backoff ownership | [ADR-0006](decisions/ADR-0006-networking-discovery-provisioning.md#decision-retrybackoff-policy-ownership) | Shared Core utility by default |
| LAN discovery shape | [ADR-0006](decisions/ADR-0006-networking-discovery-provisioning.md#decision-lan-discovery-service-shape) | Thin Core mDNS wrapper only |
| Initial Wi-Fi provisioning | [ADR-0006](decisions/ADR-0006-networking-discovery-provisioning.md#decision-initial-wi-fi-provisioning-flow) | SoftAP + captive portal, Touch UI keyboard as fallback |
| Web UI authentication | [ADR-0007](decisions/ADR-0007-web-management-ui-policies.md#decision-authentication-mechanism) | Local admin password + session login |
| Admin password sequencing | [ADR-0007](decisions/ADR-0007-web-management-ui-policies.md#decision-when-the-admin-password-is-set) | First-login sets password, not part of SoftAP flow |
| Dashboard layout model | [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-dashboard-layout-model) | Fixed grid |
| Weather data source | [ADR-0008](decisions/ADR-0008-dashboard-widget-system.md#decision-weather-data-source) | Pluggable `WeatherProvider`; Open-Meteo from M2, HA-sourced from M6 |
| Touch/display controller detection | [ADR-0009](decisions/ADR-0009-touch-display-detection.md) | Runtime detection + persisted result, not a compile-time flag |
| Secret storage | [ADR-0010](decisions/ADR-0010-secret-storage.md) | NVS encryption (HMAC-peripheral key scheme, confirmed independent of flash encryption on ESP32-P4) + hashed admin password |
| OTA image signing | [security.md](architecture/security.md#ota-image-integrity) | Known gap, deliberately deferred — not yet in scope |
| LVGL thread safety | [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md) | Dedicated UI task owns LVGL; event bus guarantees safe hand-off via `lv_async_call()` |
| Event payload lifetime | [ADR-0011](decisions/ADR-0011-lvgl-thread-safety.md#decision-event-payload-lifetime-across-the-dispatch-boundary) | Reference-counted copy at publish time, not a raw pointer into publisher state |
| Storage tiers | [ADR-0012](decisions/ADR-0012-storage-tiers.md) | NVS (credentials) / internal flash (cache, logs) / optional microSD (extended log archival only — not backups) |
| Internal flash filesystem | [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-internal-flash-filesystem-choice) | FAT + `wear_levelling` (in-tree, not third-party LittleFS) |
| Storage namespacing | [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-storage-namespacing) | Enforced per-module by the storage service, not a naming convention |
| Backup delivery | [ADR-0012](decisions/ADR-0012-storage-tiers.md#decision-backup-delivery) | Always Web UI JSON download, regardless of SD presence |
| Crash/reboot diagnostics | [ADR-0013](decisions/ADR-0013-crash-and-reboot-diagnostics.md) | Reset-reason tracking + core dump to a dedicated partition, raw download only |
| Harmony local control feasibility | [ADR-0003](decisions/ADR-0003-module-architecture.md#known-external-risk-harmony-hub-local-control) | Scoped to already-paired hubs; protocol specifics investigated in M3 |
| Module interface (exact API) | [modules.md](architecture/modules.md#status) | Deferred by design — defined when Harmony (M3) is built |
