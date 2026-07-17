# ADR-0002: Technology Stack

## Status

Accepted

## Context

CLAUDE.md fixes the high-level firmware stack: ESP-IDF, modern C++, FreeRTOS,
LVGL, M5Unified, M5GFX, with the Arduino framework explicitly excluded. It
also establishes that business logic should be shareable between the ESP32
target and a desktop simulator. It does not specify several supporting
library and tooling choices that are needed before implementation can start.
This ADR records the fixed decisions and the supporting library/tooling
decisions, together with the alternatives considered for each.

## Decision: Firmware Core (Fixed)

- **Framework:** ESP-IDF (not Arduino). Rationale: ESP-IDF gives direct
  access to FreeRTOS, power management (light/deep sleep, wake sources),
  partition/OTA APIs, and the native `esp_http_server`, without the
  additional abstraction layer and version lag that the Arduino core for
  ESP32 introduces. HomeDeck's power management and OTA requirements
  (first-class features per CLAUDE.md) are easier to implement correctly
  against ESP-IDF directly.
- **Language:** Modern C++ (C++17/C++20, exact standard pinned at M1 when the
  ESP-IDF/toolchain version is chosen). RAII, dependency injection, small
  focused types, const-correctness per the coding standards in CLAUDE.md.
- **RTOS:** FreeRTOS, as bundled with ESP-IDF. Module background tasks and
  Core scheduling are built on top of it rather than a custom scheduler.
- **UI toolkit:** LVGL. Chosen (implicitly, by being named in CLAUDE.md) over
  a bespoke rendering stack because it is mature, actively maintained,
  touch-first, has an existing SDL2-based desktop simulator story (see
  [Simulator rendering backend](#1-simulator-rendering-backend) below), and
  has first-class support in M5GFX/M5Unified for M5Stack hardware.
- **Hardware support library:** M5Unified + M5GFX for Tab5 peripheral access
  (display, touch, IMU, RTC, battery, speaker, mic). These sit at the bottom
  of the hardware abstraction layer described in
  [architecture/overview.md](../architecture/overview.md); Core and modules
  must not call them directly (see [hardware abstraction](../architecture/overview.md#hardware-abstraction)).
  **Amended for display/touch specifically by
  [ADR-0014](ADR-0014-hardware-support-library.md), and for battery/RTC
  specifically by [ADR-0016](ADR-0016-battery-rtc-library.md)**, both
  based on concrete M1 bring-up evidence rather than editing this
  decision in place per this ADR's own Consequences note below.

## Decision: Build System

**Options:**
- A single build system: consolidate firmware and simulator on ESP-IDF's
  own Linux/POSIX host target (`idf.py --preview set-target linux`), so
  both targets share one toolchain and Core code runs on FreeRTOS in both
  places — no concurrency abstraction needed.
- Two separate build systems: standard `idf.py` for firmware, and an
  independent host-native CMake project for the simulator, with an
  explicit concurrency abstraction bridging the two.

**Checked against ESP-IDF's own documentation before deciding:** the
Linux/POSIX host target is documented as designed for headless unit
testing and automation, not graphical applications — no SDL2, display, or
GUI-related component appears anywhere in its supported-component list, it
carries constraints that directly conflict with SDL2 (`printf()`
discouraged due to signal-handler issues; blocking calls like `select()`
"don't work reliably" with the simulated scheduler — both are things an
SDL2 event loop routinely depends on), and it's labeled experimental with
no API stability guarantee. It is not a viable base for a graphical
desktop simulator.

Sources: [ESP-IDF "Running Applications on Host" guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/host-apps.html).

**Decided:**

- **Firmware target:** standard `idf.py` build against the ESP32-P4
  toolchain (`idf.py set-target esp32p4`).
- **Simulator target:** a separate host-native CMake project, building
  Core/UI/module source against a host C++ compiler, host-compiled LVGL
  with its SDL2 desktop driver, and the Core-level concurrency abstraction
  described below.

## Decision: Core Concurrency Abstraction

A separate host-native simulator build means Core's background tasks,
event bus, and module lifecycle can't be built directly on FreeRTOS
primitives — a plain host C++ toolchain doesn't provide those, and this
needs an explicit resolution, not a gap.

**Decided:** a small set of Core-owned concurrency types — `Task`,
`Queue`, and `Timer` — covering what Core and modules actually use
(background tasks, the event bus's internal dispatch, and periodic/
one-shot scheduling). Core and module code is written against these types
exclusively, never against `xTaskCreate`/`xQueueCreate`/FreeRTOS APIs or
`std::thread`/host APIs directly:

- **Firmware implementation:** thin wrappers directly over FreeRTOS
  (`xTaskCreate`, queues, `xTimerCreate`) — near-zero overhead, essentially
  a naming layer.
- **Simulator implementation:** thin wrappers over the C++ standard
  library's concurrency primitives (`std::thread`, a mutex/condition-
  variable-backed queue, `std::chrono`-based timers) — no ESP-IDF
  dependency on the simulator side at all.

This is the same hardware/service abstraction pattern already used
throughout this project (display, touch, IMU, battery, HTTP server) —
Core depends on an interface, firmware and simulator each provide a
backend, and Core/module code above the interface doesn't know or care
which one it's linked against. The same Core logic runs identically on
both targets through this interface rather than by literally sharing an
RTOS underneath, which is what makes GoogleTest-against-the-simulator
(below) a genuine test of Core's real logic rather than an approximation
of it.

## Supporting Library and Tooling Decisions

The following were not specified in CLAUDE.md. Each records the options
considered and the reasoning for the choice made.

### 1. Simulator rendering backend

**Options:**
- LVGL's official SDL2 desktop driver, running the real Core/UI/module code
  natively, with a thin hardware-facing interface swapped for
  SDL2-backed input/output and stub sensors.
- A separate web-based mock UI (e.g. React) that visually approximates the
  device but does not run the real C++ code.

**Decided: SDL2 + LVGL desktop backend.** This is the only option that
satisfies CLAUDE.md's requirement that the simulator run *shared* code
rather than a re-implementation, and it is LVGL's officially supported path
for desktop simulation. A separate web mock would inevitably drift from
firmware behavior and duplicate UI logic. See
[architecture/simulator.md](../architecture/simulator.md) for the detailed
design implication.

### 2. JSON library

**Options:**
- `cJSON` — bundled with ESP-IDF, C API, minimal footprint.
- `nlohmann::json` — modern C++ ergonomics (matches the "modern C++" coding
  standard), header-only, but larger code size and slower parse than cJSON.

**Decided: `nlohmann::json`** for Core/module code and the web API layer,
given the Tab5's ESP32-P4 has substantially more flash/RAM than typical
ESP32 targets and the project explicitly prioritizes maintainable modern
C++ over minimal footprint. Revisit if flash/RAM pressure becomes a real
constraint later.

### 3. Embedded web/WebSocket server

**Options:**
- `esp_http_server` (ESP-IDF component) on firmware, paired with a
  different portable server (civetweb) on the simulator — no extra
  on-device dependency, but two different implementations behind one
  interface.
- A third-party embeddable server (e.g. civetweb, mongoose) used
  consistently on both firmware and simulator, for genuinely identical
  behavior on both targets — at the cost of not using ESP-IDF's native
  server on firmware.

**Decided:** the first option — a small abstract `HttpServer`/
`WebSocketServer` interface in Core (per the hardware/service abstraction
pattern used elsewhere), backed by `esp_http_server` on firmware and
civetweb on the simulator. The web UI's static assets and REST/WebSocket
*contract* stay identical either way, and this means the Web Management UI
can be developed against the simulator at all, which matters since the
simulator is meant to be the primary UI development environment.

**Known tradeoff, accepted deliberately:** this is the one place in the
architecture where "the simulator runs the same code as firmware" does not
hold — two different HTTP/WebSocket implementations can diverge on parsing
edge cases and framing details that the shared *contract* doesn't capture.
The alternative (civetweb on both targets) would close that gap but give up
ESP-IDF's native, zero-extra-dependency server on firmware for a
third-party dependency instead — judged not worth it, since Core's HTTP
surface (REST + WebSocket for a handful of module/admin use cases) is
narrow enough that firmware-side edge cases are unlikely to be extensive.
Web UI/API changes should still get an on-hardware check before being
considered verified, not just a simulator pass — see
[web-ui.md](../architecture/web-ui.md#transport) and
[simulator.md](../architecture/simulator.md#what-the-simulator-is-not).

**Non-UI event bus subscribers are not exempt from dispatch-safety
concerns.** A future Web UI WebSocket relay, pushing events out to
connected browsers, has no LVGL constraint but has its own equivalent one:
`esp_http_server` requires pushing data to an open WebSocket connection to
happen from the HTTP server's own task, not an arbitrary caller. ESP-IDF
provides `httpd_queue_work()` to marshal such a push onto the server's own
task — the same shape of problem `lv_async_call()` solves for LVGL, just
for a different owned resource. Whether civetweb (the simulator's server,
per the decision above) has an equivalent requirement, and if so what it
is, is **not yet confirmed** — this needs verifying during M2, and the
answer may not match `esp_http_server`'s model, which would be a second,
more specific instance of the simulator/firmware divergence already
accepted above (not just "behavior might differ," but potentially "the
safe dispatch mechanism itself differs per backend"). The event bus's
WebSocket-relay dispatch therefore needs its own safe hand-off, analogous
to but implemented separately from the UI hand-off in
[ADR-0011](ADR-0011-lvgl-thread-safety.md).

### 4. Web Management UI frontend approach

**Options:**
- Vanilla HTML/CSS/JS, no build step, hand-authored — smallest, simplest,
  slowest to develop for anything beyond basic forms.
- A modern compiled frontend (e.g. Svelte) with a build step producing a
  small static bundle — better developer experience, still compiles to a
  small footprint suitable for flash/SD-hosted static assets.
- A heavier SPA framework (React/Vue with a full runtime) — best developer
  ecosystem, largest bundle size, arguably disproportionate for an admin UI
  on an embedded device.

**Decided: Svelte + Vite**, producing a static bundle served from the
internal flash filesystem (the same FAT + `wear_levelling` partition
[ADR-0012](ADR-0012-storage-tiers.md#decision-internal-flash-filesystem-choice)
decided for cached data and logs — not a separate technology choice, and
not microSD, which ADR-0012 scopes to extended log archival, not static
assets). This keeps runtime footprint close to vanilla JS while giving a
maintainable component-based DX for what will eventually be a non-trivial
admin UI (module configuration, diagnostics, OTA, settings — not initial
Wi-Fi setup, which is a separate SoftAP captive-portal flow, see
[networking.md](../architecture/networking.md#initial-wi-fi-provisioning)).
Vanilla JS remains the fallback if bundle size proves problematic in
practice.

### 5. Test framework

**Options:**
- GoogleTest (+ GoogleMock) only, run against the host-native simulator
  build — richer C++ assertions and mocking, well suited to the
  dependency-injected, interface-based design CLAUDE.md calls for.
- GoogleTest for host-side tests, plus Unity for a separate on-target test
  suite run through ESP-IDF's own test runner.
- Catch2 instead of GoogleTest — header-only, pleasant syntax, but no
  built-in mocking support.

**Decided: GoogleTest+GoogleMock only, run against the simulator build.**
No separate Unity/on-target test suite is committed to. Given the [Core
Concurrency Abstraction](#decision-core-concurrency-abstraction) above,
Core's actual logic runs identically on both targets (the same
Core/Task/Queue/Timer-based code, against different backends), so
GoogleTest against the simulator already covers what a separate on-target
Unity suite would exist to catch. What's left that's genuinely
on-target-only (deep-sleep wake behavior, real display rendering, OTA
flash writes, real Wi-Fi co-processor reconnect timing) doesn't suit
unit-test assertions anyway — those are validated by manual/exploratory
hardware bring-up during M1/M2, not automated tests, and a second test
framework wouldn't change that. Mocking support matters for the GoogleTest
choice specifically because Core's service-interface design (module
lifecycle, event bus, storage) is explicitly meant to be testable in
isolation. If a concrete need for automated on-target testing emerges later
(timing-critical code, partition/flash edge cases), add Unity or ESP-IDF's
QEMU-based test runner then, rather than carrying an unused second
framework from M1.

## Consequences

- All decisions above (fixed and supporting library/tooling) can be relied
  upon immediately when M1 implementation starts.
- If any of the supporting library/tooling decisions need to be revisited
  once real implementation experience contradicts the reasoning here (e.g.
  civetweb proves unworkable, or nlohmann::json's footprint becomes a real
  constraint), record that as a new ADR that supersedes the relevant section
  of this one, rather than editing history in place.
