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
8. [docs/roadmap.md](docs/roadmap.md)'s M1 section — the actual task list,
   which links out to anything else specific as it comes up.

Everything else — `core.md`, `modules.md`, `web-ui.md`, `dashboard.md`,
`networking.md`, `security.md`, `diagnostics.md`, `power-management.md`,
and most of the ADRs
(0001, 0003–0008, 0010, 0012, 0013) — covers M2 platform services or later
milestones. Worth skimming for context, but nothing there blocks starting
M1, and re-reading it in full when M2 actually starts will be more useful
than trying to hold all of it in mind now.

## Required tools

- **ESP-IDF v5.4.2** — the firmware target's build toolchain (`idf.py`),
  pinned to this version. Confirmed to build for the `esp32p4` target (see
  [ESP-IDF setup](#esp-idf-setup) below for how this was verified). Not
  used for the simulator, which is a separate host-native CMake build —
  see [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#decision-build-system).
- **Docker** — runs the `espressif/idf:v5.4.2` image, the verified way to
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
  instead of Docker; the `espressif/idf:v5.4.2` image bundles its own
  (Python 3.12.3, verified working).
- **Node.js + a package manager** — required once the Web Management UI
  frontend build exists (M2). Exact tooling depends on the frontend
  framework decision in
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#4-web-management-ui-frontend-approach).

## ESP-IDF setup

**Verified working:** `esp32p4` target support, a full `idf.py build` of a
minimal project, producing a flashable `.bin`, all run through the
`espressif/idf:v5.4.2` Docker image — no native ESP-IDF install involved
(and no issue with the host's own, very new Python — the container brings
its own 3.12.3).

1. `docker pull espressif/idf:v5.4.2` (already available locally as of this
   verification).
2. From the repository root, once `firmware/` contains an actual ESP-IDF
   project (M1's "ESP-IDF project scaffolding" item — not yet done):
   ```
   docker run --rm -u "$(id -u):$(id -g)" \
     -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
     -v "$(pwd)/firmware:/project" -w /project \
     espressif/idf:v5.4.2 idf.py set-target esp32p4 build
   ```
   **The `-u "$(id -u):$(id -g)"` matters, not just style:** without it, the
   container runs as root, and every file it writes into the bind-mounted
   `firmware/` directory (the `build/` output, `sdkconfig`, etc.) comes out
   root-owned on the host — confirmed directly during this verification,
   where cleaning up a root-owned scratch build required a second Docker
   invocation just to `rm` it. Running as the host UID/GID fixes this —
   confirmed end-to-end with a full `idf.py build` producing a flashable
   `.bin` while owned by the host user throughout.

   **The `GIT_CONFIG_*` env vars matter too:** running as a non-root host
   UID means git refuses to touch `/opt/esp/idf`'s components (they're
   root-owned inside the image) and prints a "dubious ownership" warning —
   confirmed this doesn't block configure or build, but it's needless
   noise. Setting `safe.directory=*` via environment variables (rather than
   `git config --global`, which would need a writable `$HOME` the
   container doesn't reliably have for an arbitrary host UID) suppresses
   it cleanly — confirmed with a full rebuild producing an identical,
   warning-free result.
3. **Flashing** needs the device passed through to the container (e.g.
   `--device=/dev/ttyUSB0` on Linux) — not yet exercised, since it needs
   real Tab5 hardware connected. Confirm the exact flags during M1 hardware
   bring-up rather than assuming this plan is complete.

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

- **Firmware build:** `idf.py build` (and `flash`, `monitor`) against the
  `firmware/` ESP-IDF project, once it exists.
- **Simulator build:** ordinary CMake (`cmake -B build && cmake --build
  build`, or equivalent) against `simulator/`'s own host-native project,
  once it exists — a separate build system from firmware, not `idf.py`.
- **Automated tests:** GoogleTest+GoogleMock-based tests (see
  [ADR-0002](docs/decisions/ADR-0002-technology-stack.md#5-test-framework))
  run against the simulator build, for Core/module logic. No separate
  on-target test framework is used — hardware-dependent behavior (deep
  sleep/wake, display, OTA, real Wi-Fi reconnect) is validated by manual
  bring-up checks on real hardware instead, not automated on-target tests.

No test suite exists yet; this section describes the intended shape
per [ADR-0002](docs/decisions/ADR-0002-technology-stack.md) and will be
updated with real commands once M1/M2 implementation produces something to
run.

## Status

No buildable code exists yet (see [docs/roadmap.md](docs/roadmap.md) — M0
is documentation/foundation only). This document will be revised
continuously as M1 implementation makes the workflow above concrete and
verifiable.
