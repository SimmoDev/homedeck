# ADR-0016: Battery and RTC Library

## Status

Accepted

## Context

[CLAUDE.md](../../CLAUDE.md) fixes the high-level firmware stack, naming M5Unified and M5GFX
specifically for "Tab5 peripheral access (display, touch, IMU, RTC,
battery, speaker, mic)."
[ADR-0014](ADR-0014-hardware-support-library.md) already rejected
M5Unified/M5GFX categorically for this project — not just for
display/touch — because of a confirmed, unresolved crash on this exact
chip via ESP-IDF's Arduino-as-Component integration, the only way to use
it outside the Arduino IDE, which [CLAUDE.md](../../CLAUDE.md)'s "Do not use the Arduino
framework" instruction already rules out. ADR-0014 explicitly scoped its
own decision to display/touch only, and left battery and RTC as an open
question: *"`espressif/m5stack_tab5` does appear to bundle IMU and audio
support... which may mean the same reasoning extends to those peripherals
too, but that's unconfirmed, not assumed — resolve it when each is
actually brought up, not by extrapolation."*

Battery and RTC were brought up as part of M1's on-device dashboard work
(live clock, real battery percentage — see
[hardware.md](../architecture/hardware.md#on-device-dashboard)). Unlike
IMU and audio, `espressif/m5stack_tab5`'s own capability table explicitly
marks battery as unsupported and doesn't cover RTC at all — confirmed by
reading the BSP's own documentation, not assumed. Since M5Unified/M5GFX
is already off the table project-wide (per ADR-0014, not a decision this
ADR revisits), the BSP not covering these two peripherals means a real
choice was needed, not just a reuse of an already-decided answer.

## Decision

**Options considered:**
- M5Unified/M5GFX, as [CLAUDE.md](../../CLAUDE.md) originally named for these peripherals —
  not re-litigated here; already rejected project-wide by ADR-0014 for a
  confirmed crash risk, not a decision specific to battery/RTC.
- Hand-write INA226 (power monitor) and RX8130CE (RTC) drivers directly
  against ESP-IDF's native `i2c_master` API — technically possible, but
  means writing and maintaining two device drivers from scratch (register
  maps, conversion formulas, timing) with no tested reference to build
  from, the same category of risk ADR-0014 weighed against a hand-rolled
  display driver and rejected.
- **`espp/ina226` and `espp/rx8130ce`** (decided) — a third-party,
  actively maintained ESP-IDF C++ component ecosystem (`esp-cpp/espp` on
  GitHub), not Espressif's own like `espressif/m5stack_tab5`. Checked
  before depending on it, not assumed trustworthy: 959 commits and 114
  releases at decision time (latest `v1.1.6`, the version pinned below),
  MIT licensed, both components require only `idf >=5.0`, comfortably
  satisfied by the `v5.4.3` pin ADR-0014 already established.

**Decided:** `espp/ina226` and `espp/rx8130ce`, pulled as managed
components (`firmware/main/idf_component.yml`). Confirmed working on the
reference hardware: a real (not mocked) battery percentage and real RTC
time both read successfully — see
[hardware.md](../architecture/hardware.md#on-device-dashboard) for what
those reads actually showed, including two real gaps this surfaced (a
simple linear battery-percentage approximation, not true fuel-gauge
coulomb-counting; and the RTC having never been set, both pre-flagged
limitations, not library bugs). Both components communicate through
function-pointer glue matching their shared `BasePeripheral` shape rather
than a bus handle directly; a small shared `I2cDevice` helper
(`src/platform/firmware/i2c_device.h`) wraps ESP-IDF's `i2c_master`
driver once and is reused by both, rather than duplicated. Both reuse the
BSP's existing shared I2C bus (`bsp_i2c_get_handle()`) instead of
creating a second, conflicting one on the same physical pins.

This is a further deviation from [CLAUDE.md](../../CLAUDE.md)'s named libraries, in the same
spirit as ADR-0014: [CLAUDE.md](../../CLAUDE.md)'s Technology Stack section fixes
M5Unified/M5GFX as the *route* to peripheral access; for battery and RTC
specifically, that route was never viable project-wide (per ADR-0014),
and the BSP that replaced it for display/touch doesn't cover these two
peripherals either. `espp` is what actually delivers the outcome [CLAUDE.md](../../CLAUDE.md)
wants (real hardware access to these peripherals), not a rejection of the
underlying intent.

## Consequences

- This ADR amends [ADR-0002](ADR-0002-technology-stack.md#decision-firmware-core-fixed)'s
  "Hardware support library" line further — already amended once by
  ADR-0014 for display/touch; now also amended for battery/RTC
  specifically. ADR-0002's own text is left as written, per its
  Consequences section's own instruction to record changes as a new,
  superseding ADR rather than editing history in place; a pointer to this
  ADR is added at the relevant bullet.
- [ADR-0014](ADR-0014-hardware-support-library.md)'s own Consequences
  section explicitly flagged this as a follow-up to resolve when battery/
  RTC were actually brought up — **done**, with a different library than
  either M5Unified/M5GFX or `espressif/m5stack_tab5`: the BSP doesn't
  cover these peripherals, so a third, separate dependency was the
  correct outcome, not a sign the evidence-first approach failed.
- **Not resolved by this decision:** ADR-0002's hardware-support-library
  line also named IMU and speaker/mic — this ADR only covers battery and
  RTC. `espressif/m5stack_tab5` does appear to bundle IMU and audio
  support (`bsp_sensors.c`, `bsp_audio.c`), which may mean the BSP covers
  those directly without needing a third-party component like `espp`
  here, but that's unconfirmed, not assumed — resolve it when each is
  actually brought up, same as this decision was.
- New dependency footprint is larger than the two direct declarations
  suggest: `espp/ina226` and `espp/rx8130ce` each pull in
  `espp/base_peripheral` → `espp/base_component` → `espp/logger` →
  `espp/format` (which wraps libfmt) — six managed components total, not
  two, confirmed against `firmware/dependencies.lock`. No isolated
  before/after flash measurement was taken for this specific addition;
  the current full build (this plus `espressif/m5stack_tab5`, LVGL, and
  RTTI) had 32% flash free at the time of this decision — the only
  concrete data point available, not a claim that this addition
  specifically is negligible.
