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
  [ADR-0010](../decisions/ADR-0010-secret-storage.md),
  [ADR-0012](../decisions/ADR-0012-storage-tiers.md), and
  [ADR-0018](../decisions/ADR-0018-staged-security-hardening.md) for the
  full design.
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
M6 (see [roadmap.md](../roadmap.md)); [CLAUDE.md](../../CLAUDE.md)'s longer future-modules
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

Two Core services are implemented, built during M1 rather than waiting for
M2 because the dedicated-UI-task work needed them directly: the **Event
bus** (`EventBus` in `src/core/`) and **Time/date services** (`Clock`).
Both are unit-tested in `tests/`. **Navigation** also has a minimal
implementation — a route registry (`Register`/`GoTo`/`GoHome`), proven
against a deliberately throwaway second screen — though it lives in
`src/ui/`, not `src/core/`, since its `lv_scr_load()` call is a UI-layer
implementation detail (see [src/README.md](../../src/README.md) for the
same reasoning applied to `EventBus` staying LVGL-free).

**Configuration and Storage** are also implemented now (`Storage` in
`src/core/`, unit-tested in `tests/`) — the two named responsibilities
above map onto one class: schema-versioned settings/cache/secret
read-write (Configuration) backed by the NVS and internal-flash-FAT tiers
of [ADR-0012](../decisions/ADR-0012-storage-tiers.md)'s three-tier split
(Storage), namespaced per module by requiring a module ID on every call
rather than trusting callers to prefix their own keys. Secrets route
through a distinct `SecretStore`/`SetSecret`/`GetSecret` call path from
general settings, per
[ADR-0010](../decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface)
— `AdminAuthService`'s admin password hash uses it, via
`HostSecretStore`/`FirmwareSecretStore`
(`src/platform/host/`/`src/platform/firmware/`): `HostSecretStore`
stores under a separate `secrets/` directory from `HostSettingsStore`'s
`settings/`, and `FirmwareSecretStore` similarly has its own dedicated
NVS partition, separate from `FirmwareSettingsStore`'s — see
[ADR-0027](../decisions/ADR-0027-secret-store-partition-separation.md).
NVS encryption
itself is deliberately not built yet, not silently dropped: it's plain
storage for now by design — see
[ADR-0018](../decisions/ADR-0018-staged-security-hardening.md) for the
staged security model that decides when it activates. The microSD tier
is still deferred - its one named use, extended log archival past the
internal tier's bounded/rotating retention, needs that base logging
facility to exist first, which it now does (see below), but archival
to SD itself isn't wired up yet.

**Crash and reboot diagnostics** — one specific, fully-specified slice of
Diagnostics — are also implemented (`firmware/main/crash_diagnostics.cpp`):
reset reason and core-dump presence are read every boot and persisted
through `Storage`, per
[ADR-0013](../decisions/ADR-0013-crash-and-reboot-diagnostics.md).

**The general Logging responsibility is also implemented** — structured,
leveled logs (`Logger`, `src/core/logger.h`/`.cpp`), per
[ADR-0019](../decisions/ADR-0019-structured-logging.md) for the format/
rotation/storage design. Built entirely on the existing `Storage`
rather than a new platform interface, so - unlike crash/reboot
diagnostics - it's not a firmware-only mechanism; the simulator uses
the same real implementation. `Log()` persists asynchronously on a
dedicated background `Task`, batching entries that arrive close
together into a single write rather than one write per call — see
[ADR-0020](../decisions/ADR-0020-async-log-persistence.md) for the
rationale. Also the first firmware use of `Queue<T>`
([src/platform/queue.h](../../src/platform/queue.h)) for cross-task
hand-off. The rest of Diagnostics (module status, connection state,
error reporting) remains unbuilt — see
[diagnostics.md](diagnostics.md#status) for the full breakdown.

**The widget system** is implemented: `Widget` and `DashboardGrid` (`src/ui/`)
are the standard interface modules will contribute dashboard content
through — see [dashboard.md](dashboard.md#status) for the widgets built
on it so far (clock, network status, weather, notifications).
Module-contributed widgets (Harmony activity, Kodi, Uptime Kuma, Home
Assistant) remain a follow-up pending those modules existing.

**Notifications** are implemented: the urgency concept ADR-0005 requires
(`NotificationSeverity`, `src/core/notification.h`), the `EventBus`-based
publish path, and all three presentation outputs (`NotificationBanner`,
`NotificationSound`, `NotificationWidget`, all `src/ui/`) exist, with
`LowBatteryMonitor` (`src/core/`) as the first publisher — see
[ui.md](ui.md#notification-presentation) for detail. No wake-cycle
mechanism gates any of this — see
[ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md).

**Networking** is partly implemented: Wi-Fi provisioning and mDNS
self-advertisement both work on hardware (see
[networking.md](networking.md#status)); the mDNS *browsing* wrapper for
modules to discover Home Assistant/Kodi remains unbuilt, with no
consumer until one of those modules exists.

**OTA updates** are implemented: `POST /api/ota/upload`/`POST /api/ota/reboot`
(`src/core/ota_routes.h`/`.cpp`), gated by `EvaluateOtaGate()`
(`src/core/ota_gate.h`) per
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate)
on top of the A/B partition table
([ADR-0017](../decisions/ADR-0017-partition-table.md)) — see
[web-ui.md](web-ui.md#status) for the full detail. Image signing remains a
known, deliberately deferred gap — see
[security.md](security.md#ota-image-integrity).

**Power management** is implemented: `PowerManager`
(`src/core/power_manager.h`/`.cpp`) implements the full
`Active`/`Idle`/`Sleeping`/`Updating`/`Error` state model — see
[power-management.md](power-management.md#status) for the full detail.

**Weather services** are implemented: the `WeatherProvider` interface
(`src/core/weather_provider.h`/`.cpp`), with `OpenMeteoWeatherProvider` as
the direct implementation feeding `WeatherWidget` — see
[dashboard.md](dashboard.md#weather-source).

**Application lifecycle** is still just the required responsibility, not
a finalized API: module init/start/stop/teardown has no real consumer
until a module exists (see
[ADR-0003](../decisions/ADR-0003-module-architecture.md)), so this stays
deferred to M3 by design rather than designed speculatively ahead of
Harmony's concrete needs.
