# Power Management

Battery life is a primary feature of HomeDeck, not an afterthought. This
document describes the power state model required by CLAUDE.md and how
other subsystems must respect it. See
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md) and
[ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md) for the
tradeoffs and rejected alternatives behind the decisions referenced below.

**Known gap:** every timing/threshold number in this document (the 30%
OTA gate, idle/sleep timeouts) is a provisional starting point, not a
measured one — no power budget exists yet estimating real current draw
(display+backlight, ESP32-P4, ESP32-C6 co-processor) against the
confirmed 2000mAh/14.8Wh battery. These numbers should be treated as
placeholders until M1/M2 produce real on-hardware measurements to tune
them against.

## Explicit power states

The application has explicit power states rather than scattered sleep
logic:

- **Active** — screen on, full interactivity, normal background task
  activity.
- **Idle** — user hasn't interacted recently; the display dims as its
  timeout approaches, and background polling reduces in frequency.
- **Sleeping** — display off (or dimmed further than Idle), but *not*
  ESP32 deep sleep: CPU and FreeRTOS keep running, and wake-on-touch is
  polling-based rather than GPIO-interrupt-based. This matches M5Stack's
  own official Tab5 firmware, which uses the same pattern rather than a
  deep-sleep GPIO wake for touch, IMU, or RTC — see
  [ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md) for the
  finding and why. Because Core keeps running, no separate wake cycle is
  needed for anything else either — see [Notifications during
  Sleeping](#notifications-during-sleeping).
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
- Sleeping's touch-wake polling is implemented once in Core, not
  reimplemented per screen.
- Display timeout and automatic brightness are Core responsibilities, not
  something each screen manages.

## Notifications during Sleeping

Because `Sleeping` keeps the CPU and FreeRTOS running (see
[ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md)), nothing
about notification handling changes between `Idle` and `Sleeping`: Core's
event bus, module background tasks, and the notification service all keep
running exactly as they do in `Idle`. There is no periodic wake-and-check
cycle, no RTC-timed wake, and no Wi-Fi reassociation cost to reason about
for this state — the device is never disconnected in the first place.

`NotificationSeverity`'s `kDeferred`/`kAlertPriority` distinction
(`src/core/notification.h`) still exists and needs no change, but its
original justification (gating a periodic wake cycle) no longer applies —
it remains available as a presentation-layer distinction (e.g. a
different sound) whenever a real consumer needs one, per
[ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md#decision).

## Hardware capabilities involved

See [hardware.md](hardware.md) for full details and sourcing. Summary of
what's confirmed as of 2026-07:

- **Wake sources:** none of touch, IMU motion, or RTC timed wake has a
  confirmed GPIO path capable of waking the P4 from deep sleep, which is
  why `Sleeping` doesn't use deep sleep at all — see
  [hardware.md](hardware.md#power) for the schematic-traced detail and
  [ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md) for the
  design consequence.
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
battery) and real hardware. See [simulator.md](simulator.md#how-it-works)
for how power states are visually represented in the simulator (dimming,
blackout, and debug controls that force each inactivity level directly
rather than waiting out the real timeouts) so power-related UI work
doesn't require real hardware.

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

**`PowerManager` (`src/core/power_manager.h`/`.cpp`) is real** for
`Active`/`Idle`/`Sleeping` — confirmed on the K145 reference unit: the
display dims after the idle timeout, goes fully off (0% brightness)
after the sleep timeout, and restores directly to full brightness on
touch from either state. It reads user inactivity through
`UserActivitySource` (`src/platform/user_activity_source.h`, backed by
`src/ui/lvgl_user_activity_source.h`/`.cpp` on both targets) and drives
`DisplayBrightness` (`src/platform/display_brightness.h`, real PWM on
firmware via `src/platform/firmware/display_brightness.h`/`.cpp`, an
LVGL overlay on the simulator via
`src/platform/host/display_brightness.h`/`.cpp` — see
[simulator.md](simulator.md#how-it-works)). `Idle -> Sleeping` is
gated by the sleep-veto mechanism (`PowerManager::RequestSleepVeto`/
`HasActiveSleepVeto`) — `HasActiveSleepVeto()` is now consulted for
real by that transition, though `RequestSleepVeto()` itself still has
no module caller until M3+.

`Updating` is also real: `POST /api/ota/upload` publishes
`OtaUpdateStateChangedEvent` immediately around the actual flash write,
`PowerManager` transitions into `kUpdating` and back to `kActive`
accordingly, and `OnTick()` skips the Idle timeout entirely while
`kUpdating` so the display can't dim mid-write — confirmed on the K145
reference unit. The Updating state's own gate check
(`src/core/ota_gate.h`, `EvaluateOtaGate()`) is separately real and
confirmed on hardware too — the 30%/external-power condition. What
`Updating` does *not* do yet is suppress other background tasks; that's
an intentionally deferred scope decision, not a gap, since there's
barely any background-task activity today to suppress.

`Error` is now real for one of its three scoped fault types:
`CriticalBatteryMonitor` (`src/core/critical_battery_monitor.h`/`.cpp`,
structural twin of `LowBatteryMonitor`) publishes
`CriticalBatteryStateChangedEvent` once the battery crosses below its
critical threshold while not on external power (the same
threshold-OR-external-power reasoning `EvaluateOtaGate` already uses),
and `PowerManager` transitions into `kError` (and back to `kActive` on
recovery) accordingly — confirmed on the K145 reference unit through the
real `Ina226BatteryReader` path. `kError` takes priority over
`kUpdating` if both are true at once, per ADR-0005's "force a safe
shutdown right now regardless of what the UI is doing" - deliberately
interrupting an in-progress OTA write rather than deferring to it,
since `esp_ota_end()` already validates an image before it can become
bootable, so an interrupted write fails safe. Of the other two fault
types ADR-0005 originally scoped Error to cover: charging-fault
detection is a permanent limitation of this board revision, not
outstanding work - `CHG_STAT` (see [hardware.md](hardware.md#power))
can't distinguish a stalled charge from a simply-unplugged or
already-full battery, and no independent cable-presence signal exists
to disambiguate (see [roadmap.md](../roadmap.md) for the detail).
Thermal fault is the same: no battery-temperature signal exists in this
design at any level, confirmed against the schematic - the IP2326's
`NTC` pin is permanently spoofed to "normal" by a fixed resistor
network rather than fed by a real thermistor, and the battery
connector's only candidate thermistor pin is unconnected on the board.
The ESP32-P4's own die temperature was considered and rejected as a
stand-in: it reflects board/CPU heat, not battery chemistry, and
wouldn't catch a failing cell before it heats the rest of the board
(see [hardware.md](hardware.md#power)). Both are closed as permanent
hardware limitations, not outstanding work.
