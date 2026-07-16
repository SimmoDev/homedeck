# Development Guide

This document describes the intended development workflow for HomeDeck.
It's written ahead of implementation (see [docs/roadmap.md](docs/roadmap.md),
M0/M1), so some specifics — exact ESP-IDF version, exact simulator
dependencies — are marked as needing verification at the start of M1 rather
than asserted as final.

## Where to start

The architecture documentation has grown large (12 architecture docs, 13
ADRs) across M0's design work. Not all of it is relevant to starting M1 —
most of it covers M2+ services and later modules. If you're implementing
M1, this is the essential reading, in order:

1. [README.md](README.md) — what HomeDeck is, in brief.
2. [docs/architecture/overview.md](docs/architecture/overview.md) — the
   four-layer mental model, event-driven design, hardware abstraction.
3. [docs/architecture/hardware.md](docs/architecture/hardware.md) — what
   you're actually building against (needs re-verification against real
   units, per its own caveat).
4. [ADR-0002](docs/decisions/ADR-0002-technology-stack.md) — the firmware
   stack, build system, and Core Concurrency Abstraction. Foundational;
   read the Build System and Core Concurrency Abstraction decisions
   specifically.
5. [docs/architecture/simulator.md](docs/architecture/simulator.md) — the
   day-to-day dev workflow M1 needs to stand up.
6. [docs/architecture/ui.md](docs/architecture/ui.md) — navigation model
   and LVGL thread safety, both load-bearing for the first screen you
   write.
7. [ADR-0009](docs/decisions/ADR-0009-touch-display-detection.md) and
   [ADR-0011](docs/decisions/ADR-0011-lvgl-thread-safety.md) — the two
   M1-specific decisions with real implementation consequences.
8. [docs/architecture/core.md](docs/architecture/core.md) — the Event bus
   and Time/date services sections describe `EventBus`/`Clock`, which
   already exist and run (`src/core/`); the rest is still M2+ design, not
   implementation.
9. [docs/architecture/dashboard.md](docs/architecture/dashboard.md) — the
   Widget system section describes the real, currently-hardcoded
   `DashboardScreen` (`src/ui/screens/`); Customization and the general
   widget-registration interface are still M2+/M7.
10. [docs/roadmap.md](docs/roadmap.md)'s M1 section — the actual task
    list, which links out to anything else specific as it comes up.

Everything else — `modules.md`, `web-ui.md`, `networking.md`,
`security.md`, `diagnostics.md`, `power-management.md`, and most of the
ADRs (0001, 0003–0008, 0010, 0012, 0013) — covers M2 platform services or
later milestones. Worth skimming for context, but nothing there blocks
starting M1, and re-reading it in full when M2 actually starts will be
more useful than trying to hold all of it in mind now.

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
- **Python 3** — only needed on the host if using a native ESP-IDF install
  instead of Docker; the `espressif/idf:v5.4.3` image bundles its own
  (Python 3.12.3, verified working).
- **Node.js + a package manager** — required once the Web Management UI
  frontend build exists (M2). Exact tooling depends on the frontend
  framework decision in
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach).

## ESP-IDF setup

**Verified working:** `esp32p4` target support, a full `idf.py build` of a
minimal project, producing a flashable `.bin`, all run through the
`espressif/idf:v5.4.3` Docker image — no native ESP-IDF install involved
(and no issue with the host's own, very new Python — the container brings
its own 3.12.3).

1. `docker pull espressif/idf:v5.4.3` (not yet pulled on every machine —
   run this before the first build/flash after upgrading from v5.4.2).
2. From the repository root:
   ```
   docker run --rm -v "$(pwd)/firmware:/project" -w /project \
     espressif/idf:v5.4.3 idf.py set-target esp32p4 build
   docker run --rm -v "$(pwd)/firmware:/project" \
     espressif/idf:v5.4.3 chown -R "$(id -u):$(id -g)" /project
   ```
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
   IDF version.
3. **Flashing and monitoring — confirmed working** against the real Tab5
   K145 reference unit (see [docs/roadmap.md](docs/roadmap.md)'s M1 Tab5
   boot item):
   ```
   docker run --rm -it -v "$(pwd)/firmware:/project" -w /project \
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
     used for `flash` (e.g. `/repo/firmware` vs. `/project`) breaks with
     "Build directory ... configured for project ... not ...". Always use
     `-v "$(pwd)/firmware:/project" -w /project`, matching step 2 above,
     for both build and flash.
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

1. Configure and build the simulator's own CMake project (a separate build
   directory from the firmware target — a genuinely different build
   system, not just a different `idf.py set-target`; see
   [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#decision-build-system)
   for why).
2. Run the resulting binary directly on your development machine; it opens
   an SDL2 window rendering the same LVGL UI that runs on-device.
3. Iterate on Core/UI/module source directly — since this is the same
   code that firmware builds, with Core's tasks/queues/timers compiled
   against the [Core Concurrency
   Abstraction](docs/decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction)
   (FreeRTOS-backed on firmware, C++ standard library-backed here), changes
   validated here should carry over to hardware without additional
   porting, modulo the things the simulator intentionally can't reproduce
   (see [simulator.md — what the simulator is
   not](docs/architecture/simulator.md#what-the-simulator-is-not)).

## Build/test workflow

- **Firmware build:** `idf.py build` against the `firmware/` ESP-IDF
  project — see [ESP-IDF setup](#esp-idf-setup) above for the verified
  Docker-based command. `flash`/`monitor` need real Tab5 hardware, not
  yet exercised.
- **Simulator build:**
  ```
  cd simulator && cmake -B build -G Ninja && cmake --build build
  ```
  A separate build system from firmware, not `idf.py` — see
  [Simulator workflow](#simulator-workflow) above.
- **Automated tests:**
  ```
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

GitHub Actions runs on every push and PR against `main`, as three
independent workflows (separate files, not jobs within one workflow, so
each gets its own status badge — see [README.md](README.md)) mirroring
the three build/test commands above:

- [`simulator.yml`](.github/workflows/simulator.yml) — builds the
  simulator.
- [`tests.yml`](.github/workflows/tests.yml) — builds and runs the unit
  test suite; a failing test fails the job, not just a failing compile.
- [`firmware.yml`](.github/workflows/firmware.yml) — builds firmware via
  the same Docker command documented in [ESP-IDF setup](#esp-idf-setup).

All three were verified locally with [`act`](https://github.com/nektos/act)
before being relied on, the same "run it, don't just read the YAML"
standard applied to every other build command in this document.

## Status

M0 is complete (see [docs/roadmap.md](docs/roadmap.md)). M1 is in
progress: the simulator scaffold, CI, the Core Concurrency Abstraction +
`EventBus` + dedicated UI task, the initial dashboard shell (live
clock/date, battery), Navigation, and the persistent home affordance
(`src/`, exercised for real by the simulator and covered by unit tests)
all build and run, per the sections above. On real Tab5 hardware: boot,
display, and touch/display controller bring-up are all confirmed working
(see [docs/architecture/hardware.md](docs/architecture/hardware.md) and
[firmware/README.md](firmware/README.md)), but touch isn't wired into
any input handling yet, no real UI/LVGL application runs on-device yet,
and no module code exists yet — this document will keep being revised as
the rest of M1 makes it concrete.
