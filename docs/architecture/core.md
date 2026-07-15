# HomeDeck Core

Core is the set of generic services that every app/module and every UI
surface builds on. It is the layer directly below the UI in the
[architecture overview](overview.md), and it must never depend on a
specific module.

## Responsibilities

- **Application lifecycle** — startup sequencing, module init/start/stop,
  shutdown/reboot handling, built on the [Core Concurrency
  Abstraction](../decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction)
  (`Task`/`Queue`/`Timer`), never FreeRTOS or host threading APIs directly.
- **Navigation** — central screen/route registry and back/home handling
  (see [ui.md](ui.md)).
- **Dashboard** — the widget host and layout for the home screen (see
  [dashboard.md](dashboard.md)).
- **Widget system** — the standard interface modules use to contribute
  dashboard widgets, independent of any specific module.
- **Notifications** — a shared service modules publish notifications to,
  decoupled from how they're ultimately presented, with an urgency concept
  (alert-priority vs. deferred) driving where and how — see
  [power-management.md](power-management.md#notifications-during-sleeping)
  and [ui.md](ui.md#notification-presentation).
- **Event bus** — the publish/subscribe backbone connecting UI, Core
  services, and modules (see [overview.md](overview.md#event-driven-design)),
  guaranteeing safe, thread-appropriate delivery to each subscriber type
  rather than leaving that to each subscriber — see
  [ADR-0011](../decisions/ADR-0011-lvgl-thread-safety.md) and
  [ADR-0002](../decisions/ADR-0002-technology-stack.md#3-embedded-webwebsocket-server).
- **Configuration** — reading/writing user and module settings; every
  persisted blob carries a schema version field from the start, with
  migration logic deferred until a real breaking change exists to migrate
  from.
- **Storage** — persistence for configuration, cached data, and credentials,
  split across three tiers by data characteristics and namespaced per
  module — see [security.md](security.md#requirement-avoid-insecure-secret-storage),
  [ADR-0010](../decisions/ADR-0010-secret-storage.md), and
  [ADR-0012](../decisions/ADR-0012-storage-tiers.md) for the full design.
- **Networking** — Wi-Fi connection management, initial provisioning,
  connectivity status, and a thin mDNS discovery wrapper (see
  [networking.md](networking.md)).
- **Logging** — structured, leveled logging usable by Core and modules,
  including reset-reason and crash/core-dump capture across reboots — see
  [diagnostics.md](diagnostics.md).
- **Diagnostics** — module status, connection state, error reporting,
  exposed through the Web Management UI. See
  [diagnostics.md](diagnostics.md) for the full design and
  [web-ui.md](web-ui.md#diagnostics) for its presentation.
- **OTA updates** — firmware update orchestration via ESP-IDF's OTA
  partition scheme, gated on battery/power state (see
  [power-management.md](power-management.md#explicit-power-states)).
- **Power management** — the explicit power-state model (see
  [power-management.md](power-management.md)).
- **Time/date services** — RTC-backed time, independent of any module
  needing to manage it itself.
- **Weather services** — Core owns a pluggable `WeatherProvider` interface
  and the dashboard widget; the data source is user-selectable and carries
  no default cloud dependency — see [dashboard.md](dashboard.md#weather-source)
  for the full design.

## Why these are centralized

Every item above is a capability more than one module plausibly needs
(logging, storage, and the event bus obviously; but also things like
weather, which the dashboard needs directly and which a future module might
also want to react to). Centralizing them means:

- Modules don't each reimplement their own storage format, logging style, or
  polling/caching policy — consistency is structural, not a matter of
  discipline.
- Cross-cutting behavior (e.g. "reduce background activity in Idle power
  state") can be enforced in one place rather than requiring every module to
  independently cooperate.
- Core can evolve (e.g. swap the storage backend) without every module
  needing to change.

**Known gap:** the event bus and module lifecycle have no defined capacity
budget — nothing today bounds memory or scheduling cost as module count
grows. This is acceptable while only 4 modules are actually scoped through
M6 (see [roadmap.md](../roadmap.md)); CLAUDE.md's longer future-modules
list is aspirational, not committed. Revisit once module count starts
actually growing past what's scoped today, rather than guessing at a
budget now without real modules to measure against.

## Boundary with modules

Core provides the mechanism; modules provide the policy specific to their
integration. For example, Core's notification service delivers and displays
notifications; it has no concept of "Uptime Kuma monitor down" — that
concept lives entirely in the Uptime Kuma module, which constructs a
generic notification and hands it to Core. See
[modules.md](modules.md) for the full module contract.

## Status

Core's service interfaces are not yet implemented — this document describes
required responsibilities, not finalized APIs. Interface design happens
starting at M2 (Platform Services), informed by the concrete needs of the
Harmony module in M3 rather than designed speculatively ahead of a real
consumer.
