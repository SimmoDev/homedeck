# Power Management

Battery life is a primary feature of HomeDeck, not an afterthought. This
document describes the power state model required by CLAUDE.md and how
other subsystems must respect it. See
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md) for the
tradeoffs and rejected alternatives behind the decisions referenced below.

**Known gap:** every timing/threshold number in this document (the 2-5
minute wake interval, the 30% OTA gate, idle/sleep timeouts) is a
provisional starting point, not a measured one — no power budget exists
yet estimating real current draw (display+backlight, ESP32-P4, ESP32-C6
co-processor, deep sleep baseline) against the confirmed 2000mAh/14.8Wh
battery. These numbers should be treated as placeholders until M1/M2
produce real on-hardware measurements to tune them against.

## Explicit power states

The application has explicit power states rather than scattered sleep
logic:

- **Active** — screen on, full interactivity, normal background task
  activity.
- **Idle** — user hasn't interacted recently; the display dims as its
  timeout approaches, and background polling reduces in frequency.
- **Sleeping** — ESP32 deep sleep, display off. Touch and IMU motion wake
  the device (see [hardware capabilities](#hardware-capabilities-involved)),
  and a periodic RTC-timer wake additionally checks alert-priority module
  state before returning to deep sleep — see [Notifications during
  Sleeping](#notifications-during-sleeping). FreeRTOS halts between these
  windows.
- **Updating** — an OTA update is in progress; behavior favors stability
  over responsiveness (e.g. suppressing non-critical background tasks).
  Entry is gated on a minimum battery threshold (e.g. 30%, tuned in M2) or
  external USB-C power — see
  [ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-ota-batterypower-gate).
  If neither condition holds, the update is deferred with a clear
  explanation, not silently blocked.
- **Error** — scoped to power-specific faults only: critical low battery
  forcing a safe shutdown, a charging fault, or a thermal fault. General
  application/module faults go through Core's diagnostics/notifications
  service instead (see [diagnostics.md](diagnostics.md)) — see
  [ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-error-state-scope)
  for why this is scoped narrowly rather than a broad catch-all.

Every subsystem that changes behavior based on power state (background
polling frequency, display brightness, network activity) reads this single
state rather than maintaining its own notion of "should I be doing work
right now."

## Why centralized, not scattered

CLAUDE.md explicitly calls out "avoid scattered sleep logic." If each
module independently decided when to reduce activity, there would be no way
to guarantee consistent battery behavior, and every new module would need
to reimplement power-awareness correctly. Centralizing the state model in
Core means:

- Modules subscribe to power-state-change events and adjust background task
  behavior accordingly (see the [background tasks
  requirement](modules.md#what-a-module-may-provide) — respecting power
  states is a listed requirement for any module background task, not
  optional).
- Wake sources (touch, IMU movement) are configured in one place.
- Display timeout and automatic brightness are Core responsibilities, not
  something each screen manages.

## Notifications during Sleeping

Deep sleep halts FreeRTOS — nothing polls, no socket stays open, no module
background task runs, between wake windows. Most notifications generated
while the device is asleep (a Harmony disconnect, a firmware update being
available) are queued via Core's notification service and surfaced on next
wake ("3 things happened while you were away") — the device does not stay
reachable to push these immediately, matching how a physical remote already
behaves.

**Alert-priority notifications are the one exception.** Monitoring-type
alerts — starting with Uptime Kuma monitors going down, plausibly extended
to specific urgent Home Assistant events later — participate in a periodic
wake cycle instead: the RX8130CE RTC's timed interrupt wake (see
[hardware.md](hardware.md#rtc)) wakes the device on a ~2-5 minute interval
(exact value tuned in M2/M5 against real reconnect-cost and battery
measurements), it briefly reconnects (paying the Wi-Fi co-processor
reassociation cost each time — see [hardware.md](hardware.md#wireless)),
asks alert-priority modules to report current state, surfaces anything
newly bad, and returns to deep sleep. This requires Core's notification
service to carry an urgency concept and requires alert-priority modules to
expose a lightweight state-check hook distinct from their normal background
task (see [modules.md](modules.md#what-a-module-may-provide)). See
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping)
for why this shape was chosen over deferring everything or waking on a
fixed interval for all notifications.

**Unverified assumption, not just an unmeasured number:** "reassociation
cost" assumes the ESP32-C6 co-processor loses its Wi-Fi association
entirely while the P4 is in deep sleep and must fully re-associate
(handshake, DHCP/lease, any application-layer reconnect) on every wake —
not resume from a lower-power state that keeps the association alive. This
is a real hardware-topology question, not a tuning parameter: if the C6
sits on a power/SDIO domain independent of the P4's deep-sleep domain, it
could instead stay in Wi-Fi modem-sleep (associated, low duty cycle)
through the P4's sleep, changing the wake cycle's entire cost shape rather
than just its magnitude — full-reassociation cost likely dominates the
2-5 minute interval's economics; modem-sleep-resume cost likely wouldn't.
Nothing in [hardware.md](hardware.md) currently confirms which topology
the Tab5 actually has. This must be confirmed during M1 (alongside the
already-flagged need for real power measurements above), and could change
which of the two cost models this design should even be tuned against —
not just what the tuned numbers turn out to be.

## Hardware capabilities involved

See [hardware.md](hardware.md) for full details and sourcing. Summary of
what's confirmed as of 2026-07:

- **Wake sources:** BMI270 IMU (interrupt-based motion wake), RX8130CE RTC
  (timed interrupt wake), and touch, all aggregated through a dedicated
  PMS150G-U06 interrupt controller. This confirms deep-sleep wake-on-touch,
  wake-on-motion, and timed wake are real hardware capabilities, not
  assumptions — see [hardware.md](hardware.md#power) for the source.
- Display timeout and automatic brightness (ambient light behavior — no
  dedicated ambient light sensor has been confirmed on the BOM; verify
  before designing automatic brightness around one).
- **Battery monitoring:** INA226 (I2C) provides real-time voltage and
  current monitoring — better than raw ADC voltage sampling, but it is not
  a dedicated fuel-gauge/coulomb-counter IC. An accurate state-of-charge
  percentage still needs coulomb-counting or a voltage/current curve model
  implemented in firmware, not a direct read of INA226's instantaneous
  values.

These are accessed through the [hardware abstraction
layer](overview.md#hardware-abstraction), not directly, so power management
logic can run identically against the simulator (with a mocked/simulated
battery and wake sources) and real hardware. See
[simulator.md](simulator.md#how-it-works) for how power states are
visually represented in the simulator (dimming, blackout, debug-triggered
wake sources) so power-related UI work doesn't require real hardware —
deep sleep itself has no meaningful desktop equivalent, so this is a
visual simulation of observable behavior, not an attempt to actually
suspend the simulator process.

## State transition policy

The state model above defines the states; exact thresholds/triggers for
transitioning between them are implementation details tuned during M2, with
one exception: the sleep-veto mechanism, decided now because retrofitting
it later would be disruptive to every module's background task code.

**Sleep-veto mechanism:** a module may request that Core delay a transition
into Sleeping (e.g. Kodi mid-playback), as an event-based, time-limited
request — "delay sleep by up to N minutes" — which Core can and will
re-evaluate/override once the window expires, not an indefinite hold. Any
module providing background tasks must be built against this constraint
from the start. See
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-sleep-veto-mechanism)
for why this shape was chosen over no veto or an unlimited one.

**Still deferred to M2 implementation, tuned against real battery
measurements rather than decided abstractly:**
- Idle timeout duration (and whether it's user-configurable)
- Idle → Sleeping timeout duration (and whether it's user-configurable)
- Display brightness curve for automatic brightness, if the Tab5's
  available sensors support ambient light sensing

## Status

Not yet implemented. Planned for M2 (Platform Services), building on the
Tab5 boot/display/battery work done in M1. Two pieces named above are
real ahead of the rest: the urgency concept this section requires of
Core's notification service (`NotificationSeverity`,
`src/core/notification.h`) exists, since a low-battery notification
needed it; and the Updating state's own gate check (`src/core/ota_gate.h`,
`EvaluateOtaGate()`) is real and confirmed on hardware — the 30%/external-
power condition itself, not the full Updating state (background-task
suppression, entry/exit transitions), which doesn't exist yet since
there's barely any background-task activity today to suppress. The wake
cycle, and everything else in this document, remain unbuilt.
