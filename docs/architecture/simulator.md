# Desktop Simulator

The desktop simulator is the primary environment for rapid UI development,
per CLAUDE.md. It exists so UI and Core/module logic can be iterated on
without flashing a Tab5 for every change.

## Design principle

The simulator must run the **same** Core, UI, and module source code as
firmware — not a reimplementation or visual approximation. This is only
possible because business logic is written against the [hardware
abstraction layer](overview.md#hardware-abstraction) rather than directly
against the hardware BSP or ESP-IDF APIs — CLAUDE.md originally named
M5Unified/M5GFX as that BSP, but display/touch specifically use
`espressif/m5stack_tab5` instead, per
[ADR-0014](../decisions/ADR-0014-hardware-support-library.md); the
abstraction-layer principle here is unaffected either way. See
[ADR-0002](../decisions/ADR-0002-technology-stack.md#1-simulator-rendering-backend)
for why this ruled out a separate web-based mock UI.

## How it works

- **Rendering:** LVGL's SDL2 desktop driver provides the display surface and
  input handling on Linux/macOS/Windows, standing in for the on-device
  display/touch BSP (`espressif/m5stack_tab5`, per
  [ADR-0014](../decisions/ADR-0014-hardware-support-library.md)).
- **Build:** a separate host-native CMake project (see
  [ADR-0002](../decisions/ADR-0002-technology-stack.md#decision-build-system)),
  compiling Core/UI/module source against a host C++ compiler and
  host-compiled LVGL. Core's tasks/queues/timers compile against the
  [Core Concurrency
  Abstraction](../decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction)
  — a `Task`/`Queue`/`Timer` interface backed by FreeRTOS on firmware and
  the C++ standard library's threading primitives on the simulator — so
  the same Core logic compiles and runs on both targets without either one
  needing to know which backend it's linked against, the same pattern
  already used for the hardware-facing interfaces below.
- **Simulated hardware and mechanisms — one principle, several instances:**
  anything with no meaningful desktop equivalent is backed by a mock/
  simulated implementation exposed through debug controls, rather than the
  simulator attempting to actually replicate hardware or firmware behavior
  it can't (or shouldn't) replicate. This serves two purposes throughout:
  GoogleTest exercises the underlying logic in isolation, and the debug
  controls let a developer manually exercise the resulting UI without real
  hardware — the two are independent, and most instances below need both.
  - *Battery/IMU/RTC:* a battery level, IMU reading, or RTC drift that can
    be manipulated from a debug control, rather than always reporting a
    fixed value.
  - *Power states* (see [power-management.md](power-management.md)): deep
    sleep halts a real ESP32, which the simulator must not try to
    replicate by actually suspending itself — that would defeat its
    purpose as a dev tool. Idle dims the SDL2 window, Sleeping blacks it
    out while the process keeps running underneath, and debug controls
    simulate wake sources, including a manual trigger for the
    alert-priority wake cycle (see
    [power-management.md](power-management.md#notifications-during-sleeping))
    so that flow doesn't require a real multi-minute wait.
  - *Crash/reboot diagnostics:* `esp_reset_reason()` and ESP-IDF's core
    dump partition (see
    [diagnostics.md](diagnostics.md#crash-and-reboot-diagnostics)) don't
    exist outside ESP-IDF. Mock data (a fake "last reboot reason," a stub
    downloadable core dump) lets the Web UI's Diagnostics page be built
    and tested here.
  - *OTA updates:* ESP-IDF's `esp_ota_*` partition APIs don't apply to a
    desktop process. The simulator accepts a firmware image upload through
    the same Web UI contract as firmware, simulates progress and a
    forceable failure outcome, and honors the same battery/power gate from
    [ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate)
    against the mocked battery level above — so the OTA page named in
    [web-ui.md](web-ui.md#scope) can be built and tested here too.
- **Networking:** the simulator runs on a real machine with a real network
  stack, so networking, the embedded HTTP server, and the Web Management UI
  can be exercised against real (or locally mocked) external services
  during development.

## What the simulator is not

- Not a pixel-perfect hardware preview — display timing, exact touch
  latency, and real battery behavior can only be validated on actual Tab5
  hardware.
- Not a substitute for on-hardware testing before a release — it is a
  development-speed tool, not a replacement for the "every completed
  milestone should leave the repository in a releasable state" bar, which
  requires real hardware validation.
- Not a guarantee of identical behavior for the Web UI/API transport
  specifically — the simulator runs civetweb where firmware runs
  `esp_http_server` (see [web-ui.md](web-ui.md#transport) for why), so
  unlike the LVGL rendering path, this one layer can genuinely diverge
  between simulator and hardware. Web UI/API changes need a real
  on-hardware check, not just a simulator pass.

## Status

The build, the Core Concurrency Abstraction's host backend, `EventBus`,
and the dedicated UI task all exist and run for real (`Task`/`Queue`/
`Timer`/`EventBus` also have unit tests in `tests/`). The real dashboard
runs on top of them too — `DashboardScreen` (live clock, battery),
`Navigation`, and the persistent home affordance are all real,
confirmed by manually running the simulator, not just compiling — see
[dashboard.md](dashboard.md#status) and [ui.md](ui.md#status) for detail.
No module code exists yet. See
[DEVELOPMENT.md](../../DEVELOPMENT.md#simulator-workflow) for the
day-to-day workflow.
