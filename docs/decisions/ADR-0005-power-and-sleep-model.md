# ADR-0005: Power and Sleep Model

## Status

Accepted

## Context

CLAUDE.md requires explicit power states (Active, Idle, Sleeping, Updating,
Error) rather than scattered sleep logic, with battery life treated as a
primary feature. [power-management.md](../architecture/power-management.md)
describes the resulting state model. This ADR records the four decisions
within that model that involved real tradeoffs and rejected alternatives —
the sleep-veto mechanism, the alert-priority wake cycle during Sleeping, the
scope of the Error state, and the OTA power gate — so the architecture doc
can state the current design without carrying the full "why not X" reasoning
inline.

## Decision: Sleep-veto mechanism

**Options:**
- No veto — Sleeping transitions purely on a timeout, no module input.
- Unlimited veto — a module can hold off sleep indefinitely.
- Event-based, time-limited veto — a module requests a bounded delay, which
  Core can override once the window expires.

**Decided: event-based, time-limited veto.** A module (e.g. Kodi
mid-playback) may ask Core to delay entry into Sleeping, but only as a
bounded request ("delay sleep by up to N minutes") that Core re-evaluates
once the window expires — not an indefinite hold. No veto was rejected
because it would sleep the device out from under active playback, a real
regression from a physical remote. Unlimited veto was rejected because it
has no protection against a stuck or buggy module silently preventing sleep
and draining the battery, directly undercutting the battery-life priority
CLAUDE.md sets. Any module providing background tasks must be built against
this constraint from the start — see
[modules.md](../architecture/modules.md#what-a-module-may-provide).

## Decision: Alert-priority wake cycle during Sleeping

**Context:** true ESP32 deep sleep halts FreeRTOS entirely — nothing polls,
no socket stays open. This conflicts with a real product requirement:
prompt notification of monitoring-type alerts (e.g. an Uptime Kuma monitor
going down) — "find out next time you pick up the device" is not
acceptable latency for that class of alert.

**Options:**
- True deep sleep only, all notifications deferred to next wake — simplest,
  best battery life, but no timely alerting at all.
- Periodic light-sleep wake-and-check on a fixed interval, for all
  notifications — closer to real-time, but costs battery on every wake
  whether or not anything changed, and treats low-urgency notifications
  (e.g. a Harmony disconnect) with the same urgency as monitoring alerts.
- Periodic wake-and-check, scoped only to notifications a module marks
  alert-priority.

**Decided:** the third option. The RX8130CE RTC's timed interrupt wake (see
[hardware.md](../architecture/hardware.md#rtc)) wakes the device on a fixed
interval (~2-5 minutes as a starting point, tuned in M2/M5 against real
reconnect-cost and battery measurements — shorter intervals mean fresher
alerts but pay the Wi-Fi co-processor reassociation cost more often, to the
point a sufficiently short interval could cost more battery than staying
associated continuously). Only alert-priority notifications participate;
everything else stays deferred-to-next-wake. This requires Core's
notification service to carry an urgency concept (at minimum: alert-priority
vs. deferred) and requires alert-priority modules to expose a lightweight
"check current state" hook distinct from their normal background task — a
full background-task wake is heavier than this check needs to be. The exact
shape of that hook is deferred to when the Uptime Kuma module (M5) is built,
consistent with [ADR-0003](ADR-0003-module-architecture.md)'s stance against
designing module APIs ahead of a real consumer, but the *existence* of the
urgency distinction is decided now since it affects the Core notification
service's contract from M2 onward.

This is a distinct mechanism from the sleep-veto above, not a variation of
it: veto delays *entering* Sleeping for something happening right now;
alert-priority checking periodically re-enters a brief awake window *during*
Sleeping.

**Open hardware question this decision rests on:** the "reassociation cost"
above assumes the C6 co-processor loses its Wi-Fi association entirely
during P4 deep sleep. Whether that's actually true — versus the C6
possibly staying in a lower-power associated state on an independent power
domain — isn't confirmed (see
[hardware.md](../architecture/hardware.md#wireless)). This affects the
cost *model* this wake cycle should be tuned against, not just the
interval's tuned value; resolving it is M2 scope, tracked against this
decision under M2's "Power management state model" item in
[docs/roadmap.md](../roadmap.md).

## Decision: Error state scope

**Options:**
- Broad catch-all — any critical fault, power-related or not.
- Removed entirely — all faults route through Core diagnostics/
  notifications uniformly, no dedicated power state for faults.
- Narrowed to power-specific faults only.

**Decided: narrowed to power-specific faults only** — critical low battery
forcing a safe shutdown, a charging fault, or a thermal fault. General
application/module faults (a module crashing, a failed network request) are
not Error; those go through Core's diagnostics/notifications service, which
already owns error reporting (see [diagnostics.md](../architecture/diagnostics.md)).
The broad catch-all was rejected because it has no well-defined entry/exit
conditions and can't actually be implemented correctly. Removing it entirely
was rejected because it loses a clean way to represent "force a safe
shutdown right now regardless of what the UI is doing," which a critical
battery fault genuinely needs as a distinct state.

## Decision: OTA battery/power gate

**Options:**
- No gate — rely entirely on ESP-IDF's A/B partition scheme for safety.
- Require external USB-C power only.
- Require a battery threshold OR external USB-C power.

**Decided: battery threshold (e.g. 30%, exact value tuned in M2) OR
external USB-C power connected**, whichever holds, before an OTA update
starts. This protects against a brownout mid-flash on a nearly-dead device,
on top of (not instead of) the safety the A/B partition scheme already
provides. "USB-C power only" was rejected as mildly user-hostile (forcing a
cable hunt at 80% battery for no real safety benefit). "No gate" was
rejected as skipping a cheap extra layer of caution for an operation the
project explicitly wants to be trustworthy. If neither condition holds, the
update is deferred with a clear UI/notification explanation, not silently
blocked. This gate is exercised in the simulator too, against its mocked
battery level, so the Web UI's OTA page can be tested without real
hardware — see
[simulator.md](../architecture/simulator.md#how-it-works).

## Consequences

- [power-management.md](../architecture/power-management.md) states the
  resulting design (the five states, the wake-cycle mechanics, the gate
  conditions) without repeating the rejected alternatives — this ADR is
  the reference for "why," the architecture doc is the reference for
  "what."
- Modules providing background tasks or alert-priority notifications must
  be built against the veto and wake-cycle mechanisms described here from
  the start; retrofitting either later would require every existing
  module's background-task code to change.
- Exact numeric thresholds (idle timeout, sleep timeout, the 2-5 minute
  wake interval, the battery percentage gate) remain implementation
  details tuned during M2/M5 against real hardware measurements, not
  fixed by this ADR.
