# Development Guide

This document describes the development workflow for HomeDeck. See
[docs/roadmap.md](docs/roadmap.md) for current milestone status.

## Where to start

The architecture documentation has grown large (13 architecture docs, 29
ADRs) across M0's design work and the M1/M2/M3 implementation work
since. Not all of it is relevant to building a new module — most of the
M1/M2-specific bring-up detail is settled and only worth consulting when
it's actually touched. If you're implementing a module (see
[docs/roadmap.md](docs/roadmap.md) for which one is current), this is
the essential reading, in order:

1. [README.md](README.md) — what HomeDeck is, in brief.
2. [docs/architecture/modules.md](docs/architecture/modules.md) — the
   module contract every integration is built against: what a module
   may provide, why it registers with Core instead of coupling to it
   directly, and why modules never talk to each other except through the
   event bus.
3. [ADR-0003](docs/decisions/ADR-0003-module-architecture.md) — the
   reasoning behind that contract; its "Known External Risk: Harmony Hub
   Local Control" section is a worked example of scoping a module's own
   external-system risk, not a template to copy literally.
4. [docs/architecture/core.md](docs/architecture/core.md#status) — what
   Core actually offers a module to build against: the event bus,
   `Storage`/`SecretStore`, the widget system, and Notifications, all
   implemented, not just the design each originally named.
5. [docs/architecture/networking.md](docs/architecture/networking.md) —
   LAN discovery and the offline-behaviour contract a module's own
   connection state should follow.
6. [docs/architecture/web-ui.md](docs/architecture/web-ui.md) — how a
   module's own configuration page is built on the generic settings API
   (`HarmonySettings.svelte`/`WeatherSettings.svelte` are worked
   examples); see [ADR-0023](docs/decisions/ADR-0023-settings-backup-api.md)
   for the endpoint shapes. If the module has an actual credential to
   protect, `SecretStore`/[ADR-0010](docs/decisions/ADR-0010-secret-storage.md)
   and ADR-0018's NVS-encryption trigger apply - neither has fired yet
   (Harmony's local protocol has no credential at all).
7. [docs/architecture/dashboard.md](docs/architecture/dashboard.md) — the
   `Widget`/`DashboardGrid` interface, if the module surfaces status on
   the dashboard (`HarmonyWidget` is a worked example).
8. [docs/architecture/power-management.md](docs/architecture/power-management.md) —
   the sleep-veto mechanism and background-task constraints every
   module's own polling/connection-handling must respect.
9. [docs/roadmap.md](docs/roadmap.md)'s section for the current
   milestone — the actual task list, which links out to anything else
   specific as it comes up.

Everything else — the M1/M2 hardware bring-up detail in
[hardware.md](docs/architecture/hardware.md),
[simulator.md](docs/architecture/simulator.md),
[ui.md](docs/architecture/ui.md)'s Rendering/Thread-safety sections, and most of
the ADRs numbered 0009 and below plus 0011–0022 — is settled
implementation to build on top of, not something a new module changes.
Worth consulting when a specific need actually touches one of those
areas (e.g. a new background task's threading rules), not worth
re-reading in full up front.

## Required tools

- **ESP-IDF v5.4.3** — the firmware target's build toolchain (`idf.py`),
  pinned to this version specifically: v5.4.2 lacks a HAL function
  (`espressif/m5stack_tab5`'s `usb` dependency needs custom FIFO sizing,
  backported to v5.4.3) that the display bring-up work needs, while
  v5.5.x has its own confirmed DSI display bugs on this exact chip — see
  [hardware.md](docs/architecture/hardware.md#display-and-touch). Confirmed
  to build for the `esp32p4` target (see [ESP-IDF setup](#esp-idf-setup)
  below for how this was verified). Not used for the simulator, which is a
  separate host-native CMake build — see
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#decision-build-system).
- **Docker** — runs the `espressif/idf:v5.4.3` image, the verified way to
  get the ESP-IDF toolchain without installing it natively on the host.
  This is the primary supported path; a native install per Espressif's own
  "Get Started" guide remains a documented alternative for anyone who
  prefers it, but isn't what this project verifies against.
- **CMake** and **Ninja** — build system for the simulator's own host-native
  project, and (wrapped by `idf.py`) for the firmware target. These are two
  independent build definitions, not one shared one — see
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#decision-build-system)
  for why.
- **A C++17/C++20 toolchain** for the host (GCC or Clang) — used for the
  simulator build and host-side unit tests. Exact standard pinned alongside
  the ESP-IDF version decision (see
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md)).
- **SDL2 development libraries** — required for the desktop simulator's
  LVGL rendering backend (see
  [docs/architecture/simulator.md](docs/architecture/simulator.md)). Exact
  package name is platform-dependent (e.g. `libsdl2-dev` on Debian/Ubuntu).
  A plain host CMake + SDL2 build is a well-trodden combination — see
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#decision-build-system)
  for why this was chosen over running it under ESP-IDF's Linux target.
- **libcurl development package** — required for `HostHttpClient`'s
  (`src/platform/host/http_client.h`) outbound HTTP support (weather
  and any future module needing it). Exact package name is platform-
  dependent (e.g. `libcurl4-openssl-dev` on Debian/Ubuntu). System-linked
  via `find_package(CURL)`, not vendored, the same host-only-tooling
  precedent as SDL2 above.
- **Python 3** — only needed on the host if using a native ESP-IDF install
  instead of Docker; the `espressif/idf:v5.4.3` image bundles its own
  (Python 3.12.3).
- **Node.js + npm** — builds the Web Management UI's Svelte/Vite frontend
  (`webui/`). Required before building firmware or the simulator, not
  optional: both depend on `webui/dist/` existing (see [Build/test
  workflow](#buildtest-workflow) below) — CMake/`idf.py` fail with a
  clear error at configure time if it's missing, rather than an obscure
  downstream one. See
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach)
  for the framework choice and
  [ADR-0025](docs/decisions/ADR-0025-webui-static-asset-storage.md)
  for why this is a separate, explicit build step rather than
  auto-invoked from CMake.

## ESP-IDF setup

The `espressif/idf:v5.4.3` Docker image provides `esp32p4` target support,
`idf.py build`, and hardware flash/monitor — no native ESP-IDF install
involved (and no issue with the host's own newer Python — the
container brings its own 3.12.3). The firmware target builds the
HomeDeck dashboard (`EventBus`, `Clock`, `DashboardScreen`, reused
directly from `src/`, plus firmware-specific
`Task`/`Timer`/`BatteryReader`/`TimeSource` implementations in
`src/platform/firmware/`), not just a bare template — see
[docs/roadmap.md](docs/roadmap.md)'s M1 items for status.

[tools/](tools/README.md) has scripts wrapping the commands below
(`flash.sh`, `monitor.sh`, `factory-reset.sh`, `set-password.sh`) for
day-to-day use - the steps here are the full explanation of what they
do and why, worth reading at least once.

1. `docker pull espressif/idf:v5.4.3` (not yet pulled on every machine —
   run this before the first build/flash after upgrading from v5.4.2).
2. Build the Web UI bundle first if you haven't (`cd webui && npm ci &&
   npm run build` — see [Build/test workflow](#buildtest-workflow)
   below). `idf.py build` fails at CMake configure time with a clear
   message if `webui/dist/` doesn't exist yet.
3. From the repository root:
   ```sh
   docker run --rm -v "$(pwd):/project" -w /project/firmware \
     espressif/idf:v5.4.3 idf.py set-target esp32p4 build
   docker run --rm -v "$(pwd):/project" \
     espressif/idf:v5.4.3 chown -R "$(id -u):$(id -g)" /project/firmware
   ```
   **The mount covers the whole repo root, not just `firmware/`** — the
   firmware component reuses portable source directly from `../../src`
   (see `firmware/main/CMakeLists.txt`), which needs to actually be
   visible inside the container. `-w /project/firmware` keeps `idf.py`'s
   own behavior (where `build/`, `sdkconfig`, etc. end up) unchanged
   despite the wider mount. Mounting the repo root does surface a cosmetic
   git ownership warning ("detected dubious ownership") during configure —
   harmless, it only means `PROJECT_VER` can't be derived from `git
   describe`, same as before this change.

   **Two steps, not one:** the build runs as root inside the container —
   root always has a valid, writable `$HOME`, so ccache, git, and
   everything else that cares about it just works. The second command
   then reclaims ownership of whatever got written into the bind-mounted
   `firmware/` directory (the `build/` output, `sdkconfig`, etc.), which
   would otherwise be root-owned on the host.

   **Switching the Docker image tag needs a clean rebuild.** CMake's
   cache bakes in the toolchain paths from whichever image last
   configured the build — reusing an existing `build/` dir (or even just
   `sdkconfig`/`dependencies.lock`/`managed_components/`) against a
   different `espressif/idf` tag fails confusingly (a missing-compiler
   error, not an obvious "wrong image" one). Delete `build/`,
   `managed_components/`, `dependencies.lock`, `sdkconfig`, and
   `sdkconfig.old`, then `set-target` again, whenever changing the pinned
   IDF version. **The same is true whenever `sdkconfig.defaults` changes**
   (flash size, PSRAM speed, RTTI, partition table size, etc. were all
   learned this way) — an existing `sdkconfig` silently ignores updated
   defaults; only deleting it and reconfiguring picks them up.

   **`firmware/components/` vs. `firmware/managed_components/`:** the
   former is git-tracked and holds `m5stack_tab5`, a local fork carrying
   a fix (see
   [ADR-0022](docs/decisions/ADR-0022-panel-init-settle-delay.md)) —
   `firmware/main/idf_component.yml`'s `override_path` points there
   instead of fetching it. Everything else is fetched into the latter,
   which is gitignored and safe to delete for a clean re-fetch.
4. **Flashing and monitoring**, against a Tab5 K145 reference unit:
   ```sh
   docker run --rm -it -v "$(pwd):/project" -w /project/firmware \
     --device=/dev/ttyACM0 \
     espressif/idf:v5.4.3 idf.py -p /dev/ttyACM0 flash monitor
   ```
   - `/dev/ttyACM0` (not `/dev/ttyUSB0`) is expected on Linux — the P4 has
     native USB-Serial/JTAG, not a separate USB-UART bridge chip, so it
     enumerates as a CDC-ACM device. Confirm the actual node after
     plugging in (`ls /dev/ttyACM* /dev/ttyUSB*`) rather than assuming.
   - `-it` (interactive terminal) is required for `monitor` specifically —
     it stays attached and streams serial output, unlike the plain
     `build` command above. `Ctrl+]` exits; `Ctrl+C` does **not**, since
     the monitor forwards most keystrokes to the device instead of
     treating them as terminal control.
   - **The build directory must be configured with the same bind-mount
     path used for flashing.** CMake bakes the absolute build path into
     its cache — building with a different `-v .../:X` mount than the one
     used for `flash` breaks with "Build directory ... configured for
     project ... not ...". Always use `-v "$(pwd):/project" -w
     /project/firmware`, matching step 2 above, for both build and flash.
   - If flashing ever fails with "Serial data stream stopped: Possible
     serial noise or corruption" right at "Connecting...", or `monitor`
     sits at "waiting for download" instead of showing boot output, a
     manual power-button press resets the device out of it (hold ~2s
     until the green LED flashes rapidly to force download mode; a
     normal press resets into the app). Only seen once so far, not a
     routine step — automatic reset has worked on every other flash.

A native install per Espressif's own "Get Started" guide (`source`-ing the
export script in each shell, then the standard `idf.py set-target esp32p4`
/ `idf.py build` / `idf.py -p <PORT> flash monitor` workflow) remains a
documented alternative, but isn't what this project verifies against.

The simulator does **not** use `idf.py` or Docker — it's a separate
host-native CMake project, described in [Simulator
workflow](#simulator-workflow) below.

## Simulator workflow

The desktop simulator (see
[docs/architecture/simulator.md](docs/architecture/simulator.md)) is the
intended primary environment for day-to-day UI development, since it
avoids a flash/reboot cycle on real hardware for every change.

Once the simulator target exists (M1):

1. Build the Web UI bundle first if you haven't (see [Build/test
   workflow](#buildtest-workflow) below) — the simulator's CMake
   configure fails with a clear message if `webui/dist/` doesn't exist.
2. Configure and build the simulator's own CMake project (a separate build
   directory from the firmware target — a genuinely different build
   system, not just a different `idf.py set-target`; see
   [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#decision-build-system)
   for why).
3. Run the resulting binary directly on your development machine; it opens
   an SDL2 window rendering the same LVGL UI that runs on-device.
4. Iterate on Core/UI/module source directly — since this is the same
   code that firmware builds, with Core's tasks/queues/timers compiled
   against the [Core Concurrency
   Abstraction](docs/decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction)
   (FreeRTOS-backed on firmware, C++ standard library-backed here), changes
   validated here should carry over to hardware without additional
   porting, modulo the things the simulator intentionally can't reproduce
   (see [simulator.md — what the simulator is
   not](docs/architecture/simulator.md#what-the-simulator-is-not)).

## Build/test workflow

- **Web UI bundle (build this first):**
  ```sh
  cd webui && npm ci && npm run build
  ```
  Produces `webui/dist/` (`index.html`/`app.js`/`app.css`, fixed
  non-hashed names — see
  [ADR-0025](docs/decisions/ADR-0025-webui-static-asset-storage.md)),
  which both the firmware and simulator builds below embed/read.
  `npm run check` runs `svelte-check` for type errors and `npm run test`
  runs Vitest unit tests (currently `passwordValidation.ts`'s,
  `harmonyValidation.ts`'s, and `deviceNameValidation.ts`'s pure
  validation logic, `guardedAction.ts`'s double-submit guard, plus
  `api.ts`'s fetch/JSON helpers - not a full
  component-testing stack, see
  [web-ui.md](docs/architecture/web-ui.md#status) for why), both
  independently of the build. Rebuild after any change under
  `webui/src/`.
- **Firmware build:** `idf.py build` against the `firmware/` ESP-IDF
  project — see [ESP-IDF setup](#esp-idf-setup) above for the
  Docker-based command, including `flash`/`monitor` against a Tab5.
- **Simulator build:**
  ```sh
  cd simulator && cmake -B build -G Ninja && cmake --build build
  ```
  A separate build system from firmware, not `idf.py` — see
  [Simulator workflow](#simulator-workflow) above.
- **Automated tests:**
  ```sh
  cd tests && cmake -B build -G Ninja && cmake --build build
  ctest --test-dir build --output-on-failure
  ```
  GoogleTest+GoogleMock (see
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#5-test-framework)),
  its own host-native CMake project — see [tests/README.md](tests/README.md).
  Now covers `Task`/`Queue`/`Timer` (the Core Concurrency Abstraction) and
  `EventBus` for real, not just a smoke test; further Core/module tests
  arrive alongside the code they test. No separate on-target test
  framework is used — hardware-dependent behavior (deep sleep/wake,
  display, OTA, real Wi-Fi reconnect) is validated by manual bring-up
  checks on real hardware instead, not automated on-target tests.

## Continuous integration

GitHub Actions runs on every push and PR against `main`, as independent
workflows (separate files, not jobs within one workflow, so each gets
its own status badge — see [README.md](README.md)). Three mirror the
three build/test commands above:

- [`simulator.yml`](.github/workflows/simulator.yml) — builds the Web UI
  bundle (including the `svelte-check` type-check gate — see
  [Build/test workflow](#buildtest-workflow) above), then the simulator.
- [`tests.yml`](.github/workflows/tests.yml) — builds and runs the unit
  test suite; a failing test fails the job, not just a failing compile.
- [`firmware.yml`](.github/workflows/firmware.yml) — builds the Web UI
  bundle, then firmware via the same Docker command documented in
  [ESP-IDF setup](#esp-idf-setup). The type-check gate isn't repeated
  here — one CI job running it is enough.

All three were verified locally with [`act`](https://github.com/nektos/act)
before being relied on.

A fourth, [`docs.yml`](.github/workflows/docs.yml), runs
`tools/githooks/check-docs.sh` against every tracked doc/code file (push,
PR, and weekly) rather than just a commit's own diff, the way the
pre-commit hook is scoped — catching narration/wording drift in files
nobody happens to touch again. Deliberately non-blocking
(`continue-on-error`), same as the hook itself: a full-repo run also
surfaces plenty of pre-existing, legitimate usage not worth gating a
build on.

A fifth, [`secrets.yml`](.github/workflows/secrets.yml), runs
`tools/githooks/check-secrets.sh` against every tracked file (push and
PR), the same full-tree reasoning as `docs.yml`. Unlike `docs.yml`,
this one is blocking — the same reasoning the pre-commit hook already
uses to block locally on this one check alone (narrow/high-precision
patterns, and a missed leak is high-severity and hard to reverse). CI
is the only enforcement point that applies unconditionally: local hook
activation (`git config core.hooksPath tools/githooks`) is a per-clone
opt-in, and `--no-verify`/GUI clients bypass it entirely.

A sixth, [`lint.yml`](.github/workflows/lint.yml), runs the pre-commit
hook's three remaining checks
(`check-esp-idf-returns.sh`/`check-curl-timeouts.sh`/`check-esp-http-timeouts.sh`)
against every tracked file (push, PR, and weekly) — the same
"full-tree, not just a commit's diff" and non-blocking reasoning as
`docs.yml`.

## Status

See [docs/roadmap.md](docs/roadmap.md) for what's built and what's still
open. The build/test workflows above apply regardless of milestone.
