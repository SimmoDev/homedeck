# ADR-0013: Crash and Reboot Diagnostics

## Status

Accepted — the partition-table detail this ADR's Consequences flagged as
still-pending is finalized in
[ADR-0017](ADR-0017-partition-table.md).

## Context

[CLAUDE.md](../../CLAUDE.md) calls Diagnostics "a first-class feature," the same framing given
to Security before [security.md](../architecture/security.md) was written.
Diagnostics has not had equivalent design treatment — [core.md](../architecture/core.md)
and [web-ui.md](../architecture/web-ui.md) each restate [CLAUDE.md](../../CLAUDE.md)'s
requirement list without designing any of it.

One gap is specific to embedded devices and worth its own ADR: normal
"structured logs" are written during ordinary operation, but a device that
crashes, panics, hits a watchdog timeout, or browns out doesn't get a
chance to flush a normal log entry explaining why — by definition,
something went wrong badly enough that ordinary execution didn't complete.
[ADR-0005](ADR-0005-power-and-sleep-model.md) already treats brownout as a
real risk (it's the reason OTA is gated on battery/power state), but
nothing captures *why* the device last rebooted, or what it was doing at
the time, which is the diagnostic information that actually matters for a
crash.

## Decision

**Options:**
- No special crash handling — rely on a developer having a serial/UART
  connection at the exact moment of a crash. The bare ESP-IDF default; does
  nothing for a crash that happens after the device has left a dev bench.
- Reset-reason tracking only — `esp_reset_reason()` cheaply reports *why*
  the last reset happened (panic, watchdog, brownout, power-on, deep-sleep
  wake, OTA, etc.), with no partition or storage cost, but no detail on
  *what the code was doing* when it crashed.
- Reset-reason tracking **and** a core dump captured to a dedicated flash
  partition on panic, containing registers/stack/backtrace.

**Decided:** the third option. Every boot reads and logs
`esp_reset_reason()`, surfaced in [Web UI diagnostics](../architecture/diagnostics.md)
as "last reboot reason." ESP-IDF's core dump capture is enabled, writing to
a dedicated flash partition on panic. The Web UI diagnostics page offers
this as a **raw download**, not on-device symbolication — decoding a core
dump into a readable backtrace requires the exact firmware build's ELF/
symbol table, which isn't practical to ship or match on-device; a
developer decodes it off-device (`idf.py coredump-info`) against the
matching build. The dedicated partition is small (tens to a few hundred
KB, configurable) and easily affordable on the confirmed 16MB flash.

No special handling was rejected as inconsistent with "first-class"
diagnostics — it means field crashes are simply unexplainable after the
fact. Reset-reason-only was rejected as materially less useful for
actually fixing a crash: knowing a device panicked without knowing where
in the code is a much weaker starting point than a backtrace.

**Panic handler behavior:** production builds must not halt-and-wait-for-
debugger, an ESP-IDF development default. On panic, the device captures
what it can (core dump) and reboots cleanly — a consumer device that hangs
indefinitely after a crash is a worse experience than one that recovers.

## Consequences

- [diagnostics.md](../architecture/diagnostics.md) states the resulting
  design without repeating this reasoning.
- The core dump partition is a real partition-table entry, planned
  alongside the OTA A/B scheme — not a detail that can be added
  invisibly later.
- Web UI diagnostics exposes a core dump download and last-reboot-reason
  display; it does not attempt to decode or symbolicate crashes in the
  browser.
- This mechanism is firmware-only — `esp_reset_reason()` and the core dump
  partition don't exist outside ESP-IDF. The simulator provides mock data
  for the Web UI's presentation of this instead of implementing the
  mechanism itself — see
  [simulator.md](../architecture/simulator.md#how-it-works).
