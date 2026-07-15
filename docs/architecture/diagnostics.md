# Diagnostics

CLAUDE.md calls diagnostics "a first-class feature," with five specific
requirements. This document is the cross-cutting reference for how each is
addressed — the same role [security.md](security.md) plays for CLAUDE.md's
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

Not named explicitly in CLAUDE.md's requirement list, but a real gap
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

Not yet implemented. Planned for M2 (Platform Services) alongside Logging
and the Web Management UI — see [roadmap.md](../roadmap.md).
