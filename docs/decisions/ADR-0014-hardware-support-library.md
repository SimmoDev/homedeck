# ADR-0014: Hardware Support Library for Display/Touch

## Status

Accepted

## Context

CLAUDE.md fixes the high-level firmware stack, naming M5Unified and M5GFX
specifically. [ADR-0002](ADR-0002-technology-stack.md#decision-firmware-core-fixed)
carried this into a concrete decision: "Hardware support library: M5Unified
+ M5GFX for Tab5 peripheral access (display, touch, IMU, RTC, battery,
speaker, mic)." [ADR-0009](ADR-0009-touch-display-detection.md) built on
that assumption directly, requiring that display/touch bring-up "first
check M5Unified/M5GFX's existing Tab5 support" before writing new
detection code.

M1 display/touch bring-up did that check, and found concrete, not
hypothetical, evidence that M5Unified/M5GFX doesn't work cleanly for this
project on this exact chip:

- M5Unified has a confirmed, open, unresolved crash on ESP32-P4/Tab5 —
  [m5stack/M5Unified#231](https://github.com/m5stack/M5Unified/issues/231),
  a "Load access fault" panic on `M5.Display.width()`. It occurs
  specifically via ESP-IDF's Arduino-as-Component integration — the *only*
  way to use M5Unified outside the Arduino IDE, which CLAUDE.md's "Do not
  use the Arduino framework" instruction already rules out. The issue's
  own report references a second, similar crash on different M5Stack
  hardware ([#199](https://github.com/m5stack/M5Unified/issues/199)),
  pointing at the Arduino-as-Component integration generally, not
  something specific to this board.
- M5GFX's dedicated `idf-component` registry release (a non-Arduino,
  ESP-IDF-native build) predates the Tab5 entirely and doesn't support it.
  Its current Tab5-supporting releases are the same Arduino-oriented
  mainline M5Unified depends on, carrying the same crash risk.

Meanwhile, `espressif/m5stack_tab5` — Espressif's own official board
support package, pure ESP-IDF, no Arduino anywhere in its dependency
chain — does runtime I2C probing between the two known hardware revisions
(ili9881c+gt911 vs. st7123+st7123), which is exactly the detection
ADR-0009 calls for, and is actively maintained (merged support for this
exact detection logic roughly three months before this decision).

## Decision

**Options considered:**
- M5Unified/M5GFX via Arduino-as-Component, as ADR-0002 assumed —
  rejected: confirmed crash on this exact chip, no known workaround at
  decision time.
- Hand-write MIPI-DSI + ST7123 panel/touch init directly against
  ESP-IDF's native `esp_lcd` APIs, bypassing both M5GFX and any
  third-party BSP — technically possible, but real evidence this is
  genuinely hard to get right blind: Espressif's own official BSP had a
  documented "doesn't init st7123" bug during its development, and the
  ESPHome community independently hit a black-screen bug on the same
  hardware needing a corrected init sequence before it worked. Writing
  and maintaining this from scratch would mean re-deriving work that
  already exists, tested, in a maintained upstream component.
- **`espressif/m5stack_tab5`** (decided) — official, maintained, pure
  ESP-IDF, already implements the runtime detection ADR-0009 wants.

**Decided:** `espressif/m5stack_tab5`, pulled as a managed component
(`firmware/main/idf_component.yml`), for display and touch specifically.
Confirmed working on the reference hardware: real pixels, correct color,
the BSP's runtime probing independently identified the same ST7123
controller already confirmed via the unit's physical sticker, and touch
initialized successfully in the same pass. Uses the BSP's LVGL-integrated
API (`bsp_display_start()`) directly, matching this project's own LVGL
commitment, rather than bypassing it with lower-level panel APIs.

This is a deviation from CLAUDE.md's named libraries, not a rejection of
the underlying intent behind naming them. CLAUDE.md's Technology Stack
section fixes M5Unified/M5GFX as the *route* to reliable, non-Arduino Tab5
hardware support; on this exact chip, that named route doesn't currently
deliver that outcome, and a different official, non-Arduino, actively
maintained path does. Full technical detail — the crash evidence, the
esp-bsp development history, the exact packages and versions — is recorded
in [hardware.md](../architecture/hardware.md#display-driver-strategy) as
the source of truth; this ADR is the decision record, not a duplicate of
that detail.

**Self-flagged "Medium Risk" by its own author at merge time, with
limited field validation** — accepted anyway, since every alternative
carried equal or worse risk (a confirmed crash, or a hand-rolled driver
with no working reference to build from). Real hardware testing bore this
out: it worked, after fixing an unrelated ESP-IDF version incompatibility
(see [Consequences](#consequences) below), not a display-specific bug.

## Consequences

- This ADR amends [ADR-0002](ADR-0002-technology-stack.md#decision-firmware-core-fixed)'s
  "Hardware support library" line for display and touch specifically —
  M5Unified/M5GFX are not used for those two peripherals on this project.
  ADR-0002's own text is left as written, per its Consequences section's
  own instruction to record changes as a new, superseding ADR rather than
  editing history in place; a pointer to this ADR is added at the
  relevant bullet.
- **Not resolved by this decision:** ADR-0002's hardware-support-library
  line also named IMU, RTC, battery, speaker, and mic — this ADR only
  covers display and touch. **Battery and RTC are now resolved too, by
  [ADR-0016](ADR-0016-battery-rtc-library.md)** — not by
  `espressif/m5stack_tab5` (its capability table explicitly doesn't cover
  either), but by a third, separate library (`espp`), following this same
  evidence-first approach. `espressif/m5stack_tab5` does appear to bundle
  IMU and audio support (`bsp_sensors.c`, `bsp_audio.c` exist in the
  component and were part of the M1 build), which may mean the same
  reasoning extends to those peripherals too, but that's unconfirmed, not
  assumed — resolve it when each is actually brought up, not by
  extrapolation.
- **New dependency footprint accepted as a real cost, not overlooked:**
  `espressif/m5stack_tab5` unconditionally pulls in camera/USB-UVC support
  (`esp_video`, `esp_cam_sensor`, `usb`, `usb_host_uvc`) that this project
  doesn't use for display bring-up — no Kconfig option to exclude it, per
  the BSP's own manifest. Accepted as the reasonable cost of a monolithic,
  official, maintained BSP over hand-picking components; revisit if flash
  footprint or maintenance surface becomes a real problem later.
- **ESP-IDF pinned to v5.4.3, not v5.4.2**, as a direct consequence of
  adopting this BSP — its transitive `usb` dependency needs a HAL function
  ESP-IDF v5.4.2 doesn't have (backported in v5.4.3). See
  [DEVELOPMENT.md](../../DEVELOPMENT.md#esp-idf-setup) and
  [hardware.md](../architecture/hardware.md#display-driver-strategy).
- [ADR-0009](ADR-0009-touch-display-detection.md)'s prerequisite check
  ("confirm M5Unified/M5GFX's Tab5 support before writing new detection
  code") is satisfied, just with a different outcome than assumed: the
  check found a different library that already does the detection, not
  M5Unified/M5GFX doing it directly. ADR-0009's own detection design
  (I2C address probing, persisted result, manual override) is superseded
  by simply using this BSP's built-in probing — no separate detection
  code needs writing.
