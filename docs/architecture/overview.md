# Architecture Overview

## Layers

HomeDeck is organized into four layers, each depending only on the layer
below it:

```text
UI
  ↓
HomeDeck Core
  ↓
Apps / Modules
  ↓
External Services / Devices
```

- **UI** — Touch UI (LVGL, on-device) and the Web Management UI (browser,
  served by the device). See [ui.md](ui.md) and [web-ui.md](web-ui.md).
- **HomeDeck Core** — the generic services every app/module builds on:
  lifecycle, navigation, dashboard, widgets, notifications, event bus,
  configuration, storage, networking, logging, diagnostics, OTA, power
  management, time/date, weather. See [core.md](core.md).
- **Apps / Modules** — isolated integrations (Harmony, Kodi, Uptime Kuma,
  Home Assistant, ...) that use Core services and expose themselves to the
  user as Apps. See [modules.md](modules.md).
- **External Services / Devices** — the Harmony Hub, Kodi instances, Uptime
  Kuma server, Home Assistant instance, and eventually other LAN/cloud
  services a module talks to.

Dependencies point strictly downward. A module may depend on Core; Core must
never depend on a module. The UI depends on Core (and, indirectly, on
modules only through Core-mediated registration — see
[modules.md](modules.md#registration-not-coupling)), never the reverse.

## Event-driven design

State changes flow up through events rather than the UI or Core polling
modules. A representative flow:

```text
Touch Input
  ↓
Application Event
  ↓
Harmony Module
  ↓
Activity Changed Event
  ↓
UI Update
```

The event bus (a Core service) is the only sanctioned way for a module to
notify the rest of the system that something changed, and the only
sanctioned way for the UI to learn about it. This is what keeps UI
components, modules, and external services decoupled from each other — see
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md) for the UI-facing
consequences and [modules.md](modules.md) for the module-facing contract.
Modules publish from their own background `Task` (the Core Concurrency
Abstraction — see
[ADR-0002](../decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction),
FreeRTOS-backed on firmware, not something modules touch directly); the
event bus itself guarantees safe delivery to LVGL-based UI subscribers
rather than requiring every module to know about UI threading — see
[ADR-0011](../decisions/ADR-0011-lvgl-thread-safety.md).

## Hardware abstraction

The Tab5 is the primary and only currently-supported target, but Core and
modules must not call the hardware BSP (M5Unified/M5GFX, or — for
display/touch specifically — `espressif/m5stack_tab5`, per
[ADR-0014](../decisions/ADR-0014-hardware-support-library.md)) or any
ESP-IDF API directly. Hardware access is mediated through a thin
hardware-facing interface layer, for two reasons:

1. It is what makes the [desktop simulator](simulator.md) possible — the
   same Core/module/UI code runs against a desktop-backed implementation of
   that interface instead of real hardware.
2. It keeps the door open for future hardware variants without a rewrite,
   per [CLAUDE.md](../../CLAUDE.md)'s hardware abstraction requirement, even though no
   alternate hardware target is planned or committed to today.

This abstraction is intentionally minimal — it should expose what Core and
modules actually need (display surface, touch events, IMU readings, battery
state, audio out, storage), not a general-purpose HAL designed for hardware
that doesn't exist yet.

## Where things live in the repository

See the top-level [README.md](../../README.md#repository-structure) for the
full repository layout. In brief: `src/` holds the portable Core/UI/
module source shared by `firmware/` and `simulator/`, which differ only
in how they're built and which hardware-facing implementation they link
against; `webui/` is the Web Management UI frontend; `hardware/` holds
any physical accessory/enclosure design work; `tools/` and `tests/` are
supporting tooling and test suites.

## Related documents

This list is exhaustive, not prioritized — most of it is M2+ scope. If
you're building a module, see [DEVELOPMENT.md's "Where to
start"](../../DEVELOPMENT.md#where-to-start) for the actually-relevant
subset and reading order instead of working through this list top to
bottom.

- [core.md](core.md) — Core services in detail
- [modules.md](modules.md) — module contract and boundaries
- [ui.md](ui.md) — Touch UI
- [dashboard.md](dashboard.md) — dashboard and widget system
- [web-ui.md](web-ui.md) — Web Management UI
- [power-management.md](power-management.md) — power states
- [networking.md](networking.md) — networking and connectivity
- [simulator.md](simulator.md) — desktop simulator design
- [hardware.md](hardware.md) — Tab5 hardware reference (chip/BOM
  facts — not to be confused with the [hardware/](../../hardware/)
  directory described above)
- [harmony.md](harmony.md) — the Harmony module (connection, activities,
  devices/remote control, status/notifications, Web UI)
- [security.md](security.md) — cross-cutting security requirements and
  where each is addressed
- [diagnostics.md](diagnostics.md) — cross-cutting diagnostics requirements
  and where each is addressed, including crash/reboot diagnostics
- [docs/decisions/](../decisions/) — the decisions behind this
  architecture
