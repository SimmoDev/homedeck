# ADR-0024: Sleeping's Wake Mechanism

## Status

Accepted. Supersedes
[ADR-0005](ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping)'s
periodic RTC wake cycle during `Sleeping`.

## Context

[ADR-0005](ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping)
decided that `Sleeping` would use true ESP32 deep sleep, woken periodically
by the RX8130CE RTC's timed interrupt to briefly check alert-priority
notification state before returning to deep sleep, with touch and IMU
motion as the other two intended wake sources. Two open hardware questions
were later logged against that decision: schematic tracing
([hardware.md](../architecture/hardware.md#power)) found no confirmed P4
GPIO path for touch, IMU, or RTC capable of using the standard ESP-IDF deep
sleep wake APIs (`esp_sleep_enable_ext1_wakeup_io()` and friends require
the RTC-IO domain, GPIO0-15; the only two P4-side pins found near these
peripherals, GPIO35 and GPIO23, sit outside it).

Resolving that by hardware investigation (multimeter/scope tracing of the
BMI270/RX8130/PMS150G pins) isn't something this project has someone
available to do. Rather than leave the decision blocked indefinitely,
M5Stack's own official Tab5 firmware (`M5Tab5-UserDemo`,
`platforms/tab5/main/hal/components/hal_power.cpp`) was checked instead —
M5Stack had the real schematic when they wrote it. It resolves the question
directly: their firmware doesn't use ESP32 deep-sleep GPIO wake for touch,
IMU, or RTC either.

- `sleepAndTouchWakeup()` dims the display and polls the touch controller
  (`esp_lcd_touch_read_data()`) in a loop until a press is detected. The
  CPU stays fully active the entire time — this is not sleep in the
  ESP-IDF sense at all.
- `sleepAndRtcWakeup()` clears pending RTC/IMU interrupts, sets an RX8130
  alarm, then calls `powerOff()`, which fully powers down the board via
  the PMS150G-U06 power controller. The device cold-boots when the alarm
  later fires. This is not a resumable wake — FreeRTOS state is not
  preserved.

No official code path uses `esp_sleep_*` deep-sleep GPIO wakeup for any of
these three sources on this hardware. This doesn't contradict the schematic
tracing — it confirms it. M5Stack's own engineers, working from the real
schematic, built around the same GPIO0-15 constraint by not depending on
it, rather than by using a wiring path this project's tracing missed.

## Decision

**`Sleeping` does not use ESP32 deep sleep.** It means: display off (or
dimmed further than `Idle`), CPU and FreeRTOS continue running normally.
Wake-on-touch is implemented the way the vendor's own firmware implements
it — polling the touch controller with the display off — not through a
GPIO deep-sleep wake source.

Because Core keeps running through `Sleeping` exactly as it does through
`Idle`, the periodic RTC-wake-and-check mechanism ADR-0005 decided on for
alert-priority notifications is no longer needed at all: nothing has
stopped running that would need waking. Alert-priority notifications are
simply handled immediately, the same way they would be in `Idle`.

This decision **replaces** ADR-0005's "Alert-priority wake cycle during
Sleeping" decision (the periodic ~2-5 minute RTC-wake mechanism) in full.
The rest of ADR-0005 — the sleep-veto mechanism, the `Error` state scope,
and the OTA power gate — is unaffected and stands as decided.

`NotificationSeverity`'s existing `kDeferred`/`kAlertPriority` distinction
(`src/core/notification.h`) needs no code change as a result of this ADR.
It's already shipped, already carries no differential behavior today (per
its own file comment — no consumer branches on it yet), and remains a
reasonable place to hang a future presentation difference (e.g. a distinct
sound) if one is ever needed. It simply no longer has any wake-cycle
behavior to gate, because none exists.

A further, smaller consequence: the still-open question of whether
ESP-Hosted/SDIO can keep the C6 co-processor associated while the P4
sleeps ([hardware.md](../architecture/hardware.md#wireless)) is now moot
for `Sleeping` specifically — the P4 never sleeps in this design, so
nothing disconnects. It isn't resolved in general; it would need its own
investigation if a future full-power-off feature (below) is ever built.

## Options considered

- **Keep pursuing a resumable GPIO-wake deep sleep for `Sleeping`,**
  investigating further to find a path the schematic trace missed —
  rejected. The vendor's own firmware, written against the real schematic
  by people who could probe the board directly, doesn't use one either.
  Continuing to chase this would mean assuming M5Stack's engineers missed
  something this project also can't confirm by inspection.
- **Adopt the vendor's touch-wake pattern (CPU stays active, poll,
  display off) for `Sleeping`, and treat the vendor's power-off/RTC-wake
  pattern as a separate, later feature, decoupled from `Sleeping`** —
  decided. It's a proven pattern that doesn't block on hardware
  investigation nobody on this project is equipped to do, and it keeps
  the standalone power-off feature's own (undesigned) scope from blocking
  `Sleeping`'s comparatively simple redefinition.
- **Build the PMS150G power-off/RTC-cold-boot-wake driver now, and make
  `Sleeping` equivalent to that "off" state** — rejected for now. The
  pulse timing for controlling PMS150G isn't in any official spec, only
  reverse-engineered by a community project; it's a materially bigger and
  riskier feature (a from-scratch hardware driver, full state loss on
  every wake) than redefining `Sleeping`, and isn't needed to unblock
  that redefinition. Tracked separately — see
  [docs/roadmap.md](../roadmap.md)'s M7 section.

## Consequences

- [power-management.md](../architecture/power-management.md)'s `Sleeping`
  description, "Notifications during Sleeping" section, and hardware
  capabilities summary are updated to match: no ESP32 deep sleep, no GPIO
  wake source dependency, CPU stays active through `Sleeping`, no
  periodic wake-and-check cycle.
- [docs/roadmap.md](../roadmap.md)'s Power management item no longer
  describes `Sleeping` as blocked on hardware investigation — the vendor
  firmware finding closes that question. A new, separate, explicitly
  future item tracks the PMS150G power-off/RTC-cold-boot-wake pattern
  under M7.
- [ADR-0005](ADR-0005-power-and-sleep-model.md)'s Consequences section
  notes this supersession; its own "Alert-priority wake cycle" decision
  text is left as the historical record of what was originally decided
  and why, not rewritten in place.
