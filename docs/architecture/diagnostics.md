# Diagnostics

[CLAUDE.md](../../CLAUDE.md) calls diagnostics "a first-class feature," with five specific
requirements. This document is the cross-cutting reference for how each is
addressed — the same role [security.md](security.md) plays for [CLAUDE.md](../../CLAUDE.md)'s
Security section, which uses the same "first-class" framing.

## Requirement: structured logs

"Structured" means each log entry carries a timestamp, level, and
module/component tag, not just a free-text message — this is what makes
filtering by module or severity in the Web UI possible, rather than
grepping an undifferentiated stream. Logs live on the internal flash
filesystem tier (see [ADR-0012](../decisions/ADR-0012-storage-tiers.md))
with bounded, rotating retention, consistent with the cache-retention
requirement in [networking.md](networking.md#offline-behaviour) — logs are
useful recent history, not an unbounded archive on that tier. Older
history that would otherwise be evicted can optionally be archived to
microSD if a card is present, per
[ADR-0012](../decisions/ADR-0012-storage-tiers.md#decision) — the one
concrete use case that tier actually has.

## Requirement: module status

Exposes each module's current lifecycle state — the same init/start/stop/
teardown lifecycle [ADR-0003](../decisions/ADR-0003-module-architecture.md)
already defines modules as supporting — plus its last error, if any. This
isn't a new concept; it's surfacing the lifecycle state that already exists
per the module contract, not a separate status model modules need to
maintain themselves.

## Requirement: connection state

Two distinct things share this name and shouldn't be conflated: device-level
Wi-Fi connectivity (see [networking.md](networking.md#responsibilities)) and
each module's connection to *its* external service (e.g. "Harmony Hub:
connected," "Kodi: reconnecting"). Both are surfaced in Web UI diagnostics,
but they're independent — a module can be disconnected from its service
while the device is fully online, and the UI needs to distinguish which is
which.

## Requirement: error reporting

Errors flow through Core's notification/diagnostics service, which is also
where [ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-error-state-scope)
routes general application/module faults that aren't power-specific. A
module reports an error the same way regardless of whether it's a minor,
transient failure or something a user should be notified about — severity
and presentation are Core's concern, not the module's.

## Requirement: debug information through the Web UI

The Web UI (see [web-ui.md](web-ui.md#diagnostics)) is the sole presentation
surface for all of the above — logs, module status, connection state, error
history — plus the crash/reboot diagnostics described below. The Touch UI
does not surface diagnostic detail; this is explicitly a Web UI concern per
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md#two-interfaces-two-different-design-goals).

## Crash and reboot diagnostics

Not named explicitly in [CLAUDE.md](../../CLAUDE.md)'s requirement list, but a real gap
without it: normal structured logs don't survive a crash, panic, watchdog
timeout, or brownout, since those are exactly the situations where ordinary
execution — including flushing a log entry — doesn't complete. Every boot
records why the *previous* boot ended (`esp_reset_reason()`: panic,
watchdog, brownout, power-on, deep-sleep wake, OTA, etc.), and a panic
additionally captures a core dump (registers/stack/backtrace) to a
dedicated flash partition, downloadable raw from Web UI diagnostics for
off-device analysis. See
[ADR-0013](../decisions/ADR-0013-crash-and-reboot-diagnostics.md) for why
this shape was chosen over reset-reason-only or no crash handling at all.

**Downloading the core dump may trigger a browser "insecure download"
warning** — plain-HTTP downloads from a local-network admin interface
trip some browsers' download-protection heuristics regardless of file
type or headers; there's no server-side header or config that
suppresses it. Expected, not a bug: fixing it means HTTPS, which reopens
[ADR-0007](../decisions/ADR-0007-web-management-ui-policies.md)'s
already-settled plain-HTTP-on-the-LAN decision for a rare, admin-only
action - not judged worth it. Click through ("Keep") to download.

**Firmware-only mechanism, not something the simulator implements.**
`esp_reset_reason()` and ESP-IDF's core dump partition don't exist outside
ESP-IDF — a simulator process crashing is an ordinary host crash (segfault,
uncaught exception), handled by the OS/debugger, not by any of this. What
the simulator *does* need is mock data for this mechanism's Web UI
presentation (a fake "last reboot reason," a stub downloadable core dump),
consistent with the simulator being where the Web UI actually gets
developed and tested — see
[simulator.md](simulator.md#how-it-works). Without that, the Diagnostics
page's crash/reboot section would be one of the only parts of the Web UI
with no way to be exercised before real hardware exists.

## Status

**Crash and reboot diagnostics are implemented**, confirmed on real hardware
including a real triggered panic (`firmware/main/crash_diagnostics.cpp`):
a deliberate `abort()` produced a real panic printout, the device
rebooted cleanly rather than halting (repeatedly, across several cycles),
and the next boot correctly logged `Last reset reason: panic` and
detected the core dump (valid checksum, correct crashing task name and
program counter). Every boot reads
`esp_reset_reason()` and checks for a core dump from a preceding panic,
logging and persisting both via [Storage](core.md#responsibilities) (see
[ADR-0013](../decisions/ADR-0013-crash-and-reboot-diagnostics.md)).

**Web UI presentation of crash/reboot diagnostics is also real**
(`src/core/diagnostics_routes.h`/`.cpp`, `webui/src/lib/Diagnostics.svelte`)
— `GET /api/diagnostics` (reset reason, core dump presence, plus live
battery/external-power state - see [hardware.md](hardware.md#power) -
added to this same endpoint for convenience rather than a separate one,
not itself part of crash/reboot diagnostics) and
`GET /api/diagnostics/coredump` (the raw file, downloadable, not decoded
in-browser per [ADR-0013](../decisions/ADR-0013-crash-and-reboot-diagnostics.md)),
both admin-only via `AdminAuthService::RequireAuth()`. The simulator
writes mock values (`reset_reason: "power-on"`, a stub downloadable
blob) into `Storage` at startup exactly as this document's own
"Firmware-only mechanism" note above calls for, so the same request-
handling code path is exercised identically on both targets — only the
mock-vs-real data source differs. Confirmed end to end on the simulator,
including a real click-driven browser session (Chrome DevTools Protocol)
proving the actual page transitions from login to the rendered
diagnostics view, not just that the API returns correct JSON.
**Confirmed on real hardware too** (Tab5 K145 reference unit, over the
LAN) — `resetReason` reflecting the device's actual last reset, and a
real core dump downloading as genuine ELF-format bytes via
`esp_core_dump_image_get()` + `esp_flash_read()` against the `coredump`
partition.

**Structured logs are also real** (`Logger`, `src/core/logger.h`/`.cpp`,
`GET /api/diagnostics/logs`) — JSON-lines entries (timestamp, level,
component, message) persisted through `Storage`'s existing cache tier
with size-based rotation, per
[ADR-0019](../decisions/ADR-0019-structured-logging.md) for the full
format/rotation design. Unlike crash/reboot diagnostics, this isn't a
firmware-only mechanism - the simulator runs the same real `Logger`,
not mock data. `Log()` persists asynchronously on a background task,
batching entries that arrive close together into one write — see
[ADR-0020](../decisions/ADR-0020-async-log-persistence.md) for the
full design rationale. **Confirmed on real hardware** (Tab5
K145 reference unit):
real boot-sequence events (Wi-Fi connect, mDNS advertising, Web UI
listening) appear correctly through the endpoint, including entries
that landed in the same batch sharing an identical persisted timestamp
where they genuinely happened at the same moment. The Web UI's Logs
section (`webui/src/lib/Diagnostics.svelte`) fetches once and filters
by level/component client-side; confirmed against the simulator's real
HTTP API and a real page load, though not yet via the same
click-driven browser session the crash/reboot diagnostics view above
was confirmed with. Extended log archival to microSD past the internal
tier's bounded retention - the one concrete use
[ADR-0012](../decisions/ADR-0012-storage-tiers.md) names for that tier
- is not built.

Still not implemented: module status and connection state (no modules
exist to report on yet). Error reporting's mechanism is ready —
Notifications is implemented (see [core.md](core.md#status)) — but nothing
publishes an error through it yet, since no module exists to report
one.
