# Tab5 Hardware Reference

Concrete hardware facts about the M5Stack Tab5, compiled from M5Stack's
official documentation and product pages (2026-07). This exists because
several earlier architecture docs referenced "the Tab5's IMU," "the Tab5's
touch controller," etc. in the abstract without recording what they actually
are — decisions like JSON library choice (flash/RAM headroom) and
wake-from-sleep design were being made against assumptions rather than
verified facts.

**This needs re-verification against the actual unit(s) in hand during M1
bring-up**, and updated here if anything drifts — M5Stack has already
revised this hardware twice during its production life (see [Display and
touch](#display-and-touch) below), so "confirmed 2026-07" is not a
permanent guarantee.

## Application processor

- **ESP32-P4**, dual-core RISC-V, 400MHz. No integrated radio (Wi-Fi/BT) —
  see [Wireless](#wireless) below.
- 16MB Flash, 32MB PSRAM. **Correction:** not Octal — the ESP32-P4 has no
  Quad/Octal PSRAM choice at all (unlike other Espressif chips); its only
  mode is what ESP-IDF's own Kconfig and boot log both call "Hex"
  (16-line), a P4-specific memory controller mode. `firmware/sdkconfig.defaults`
  reflects this (just `CONFIG_SPIRAM=y`, no mode override needed).
- **Confirmed via first real boot** (minimal `app_main()`, no display/UI —
  see [roadmap.md](../roadmap.md)'s M1 Tab5 boot item, and
  `firmware/sdkconfig.defaults`): chip revision v1.3, 16MB flash
  physically detected, 32MB PSRAM detected and initialized (vendor "AP",
  generation 4) with ~33.5MB reported free. Boots at 360MHz by default
  (400MHz is the rated max, not necessarily the default without explicit
  clock config — not changed, since nothing currently needs the higher
  clock).
- **PSRAM speed raised from 20MHz to 200MHz** (`sdkconfig.defaults`) —
  20MHz is Kconfig's unmodified default, and caused a continuous DMA
  underrun once display bring-up actually drove the panel (see
  [Display and touch](#display-and-touch) below), confirmed fixed by this
  change. These are the *only* two speeds ESP-IDF exposes for this chip's
  PSRAM controller — no 100MHz or other intermediate option exists
  despite what the Kconfig help text implies. 200MHz requires
  `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`, ESP-IDF's own marker that this
  isn't a fully hardened path — confirmed working on this unit, but worth
  remembering it's explicitly experimental if anything unrelated gets
  flaky later.

## Wireless

- **ESP32-C6-MINI-1U**, acting as a wireless co-processor for the P4 (Wi-Fi
  6, BLE, Thread, Zigbee). This confirms the dual-chip architecture flagged
  as a risk during architecture review: Wi-Fi is not native to the
  application processor, it runs through a companion chip over a host-slave
  link. See [networking.md](networking.md) and
  [power-management.md](power-management.md) for the consequences (reconnect
  cost on wake, OTA scope question for the co-processor's own firmware).
- **Link confirmed: SDIO** (`SDIO2_D0`–`D3`, `SDIO2_CMD`, `SDIO2_CK`),
  running ESP-Hosted over that bus, per M5Stack's official BSP source
  (`m5stack_tab5.c` in
  [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo)).
- **Power domain — partially confirmed.** The C6's power enable
  (`WLAN_PWR_EN`) is bit 0 of a dedicated I2C GPIO expander output
  (PI4IOE5V6408, I2C address `0x44` — see the address map above),
  toggled in software via `bsp_set_wifi_power_enable()`. No automatic
  hardware coupling to the P4's own sleep state exists in that BSP source
  — the C6's power rail is independently switchable, not wired to
  collapse whenever the P4 sleeps. This answers the specific "are they
  wired together" question, but **does not by itself confirm** the
  practical question the alert-priority wake cycle actually cares about:
  whether ESP-Hosted/SDIO can meaningfully keep the C6 "associated, low
  duty cycle" while the *P4 itself* is in deep sleep and unable to
  service the SDIO link — that's a protocol/software behavior question,
  not a wiring one, and still needs real testing during M2/M5's power
  work, not assumed from this alone. See
  [power-management.md](power-management.md#notifications-during-sleeping)
  for where this feeds into the wake-cycle cost model.

## Display and touch

- 5" 1280×720 IPS, MIPI-DSI interface.
- **Known hardware revision fragmentation:** the original display/touch
  driver pairing was ILI9881C (display) + GT911 (touch controller, I2C
  address 0x14). M5Stack replaced this with an integrated ST7123
  display+touch driver (I2C address 0x55) starting 2025-10-14, then again
  with ST7121 (also 0x55) starting 2026-04-28. Units purchased at different
  times may run different silicon here, and testing on one unit does not
  validate behavior on all of them.
- **Resolved:** the HAL detects which controller is present at runtime
  (I2C address probing, `0x14` vs. `0x55`, with a chip-ID register read to
  disambiguate ST7123 from ST7121 if one exists) and persists the result,
  rather than assuming a single controller via a compile-time flag — see
  [ADR-0009](../decisions/ADR-0009-touch-display-detection.md) for why a
  compile-time approach was rejected (it breaks the single-OTA-image
  model) and what's still unconfirmed (the chip-ID register, and whether
  ST7123/ST7121 need genuinely separate driver paths).
- **Confirmed:** the project's reference unit uses the **ST7123**
  integrated display+touch driver (I2C address 0x55) — read directly off
  the physical unit's sticker, no probing needed for this fact. The
  runtime I2C-probing detection logic itself is still not built (M2/
  display-bring-up scope, per ADR-0009 — a single reference unit knowing
  its own driver doesn't remove the need for runtime detection across
  units in the field).
- **M5Stack's own I2C address map for Tab5** (SCL: GPIO32, SDA: GPIO31),
  confirming several facts elsewhere in this document at once: ES8388
  (0x10), GT911 (0x14, not present on this unit), RX8130CE (0x32), SC2356
  (0x36), ES7210 (0x40), INA226 (0x41), PI4IOE5V6408-1/2 (0x43/0x44, I/O
  expanders not otherwise recorded here), ST7123 (0x55), BMI270 (0x68).

### Display driver strategy

See [ADR-0014](../decisions/ADR-0014-hardware-support-library.md) for the
decision record (options considered, why, consequences); this section is
the detailed technical evidence and results that decision summarizes.

**M5Unified/M5GFX's Arduino-based path is avoided, not used.** M5Unified
has a confirmed, open, unresolved crash on ESP32-P4/Tab5 specifically
when used via ESP-IDF's Arduino-as-Component integration (the only way to
use it outside the Arduino IDE, which CLAUDE.md rules out) —
[m5stack/M5Unified#231](https://github.com/m5stack/M5Unified/issues/231),
a "Load access fault" panic on `M5.Display.width()`. The issue itself
notes it isn't specific to this chip (references
[#199](https://github.com/m5stack/M5Unified/issues/199), the same crash
pattern on a different board), pointing at the Arduino-as-Component
integration generally, not this hardware.

**Decided instead: `espressif/m5stack_tab5`**, Espressif's own official
BSP component (pure ESP-IDF, no Arduino at all), pulled via the component
manager (`firmware/main/idf_component.yml`). As of v1.2.0~1 it does
runtime I2C probing between the two known hardware revisions
(ili9881c+gt911 vs. st7123+st7123) — the exact detection ADR-0009 calls
for, effectively provided by this dependency rather than hand-written.
Uses its LVGL-integrated API (`bsp_display_start()`) directly, matching
this project's own LVGL commitment, rather than bypassing it with
lower-level panel APIs. Self-flagged "Medium Risk" by its own author at
merge time with limited field validation — expect this may need real
debugging on first hardware test, not just a config tweak.

**ESP-IDF pinned to v5.4.3, not v5.4.2 or v5.5.x.** `m5stack_tab5`
unconditionally pulls in `usb` (for camera/UVC support this project
doesn't use for display bring-up) as a transitive dependency, which calls
a HAL function (custom FIFO sizing) that doesn't exist in ESP-IDF v5.4.2
— confirmed by checking IDF's own `hal` component source directly, not
assumed. That function was backported in v5.4.3 (a minor patch release),
which resolves the build without needing to jump to v5.5.x — which has
its own confirmed, unrelated DSI display regression on this exact chip
([espressif/esp-idf#18083](https://github.com/espressif/esp-idf/issues/18083):
empty screen / horizontal stripe artifacts, working on v5.4.2 and broken
on v5.5.x). See [DEVELOPMENT.md](../../DEVELOPMENT.md#esp-idf-setup) for
the current pinned version and setup instructions.

**First real result, confirmed on hardware:** a solid color fill
displays correctly on the physical panel. `espressif/m5stack_tab5`'s
runtime probing independently detected "board version 2 (LCD ST7123,
Touch ST7123)" — matching the sticker-confirmed controller above without
being told. Touch also initialized successfully in the same pass
(10-point multitouch), ahead of the roadmap's separate touch bring-up
item.

**DMA underrun — resolved.** The first test hit a continuous
`lcd.dsi.dpi: can't fetch data from external memory fast enough, underrun
happens`, logged every frame, not a one-off. Fixed by raising PSRAM speed
from Kconfig's default 20MHz to 200MHz — see
[Application processor](#application-processor) above for the exact
`sdkconfig.defaults` change. Confirmed clean on hardware: no more
underrun logs, and the solid-color fill's actual color rendered correctly
for the first time (it had visibly appeared wrong — cyan instead of the
configured blue — on the underrun-affected first test, consistent with
the underrun corrupting pixel data, not just a coincidental color choice).

**Still genuinely open, not a bring-up blocker:** reported resolution is
`720x1280` (portrait), not `1280x720`. This is the BSP's default panel
orientation, not something this project chose — real input for the
still-open landscape-vs-portrait decision, but doesn't resolve it by
itself; whether this is the panel's native scan direction or just this
driver's default init orientation isn't confirmed yet.

**Touch confirmed working end to end, not just controller init.**
`bsp_display_start()` already wires the touch controller into LVGL as a
real input device (`lvgl_port_add_touch()`, called internally — no extra
plumbing needed on this project's side). Verified on hardware with a real
on-screen touch handler: tapping the panel logs real coordinates, all
within the confirmed `720x1280` bounds, and visibly reacts (a color
toggle). `LV_EVENT_PRESSED` fires once per discrete touch-down, confirmed
by holding and dragging a finger on hardware — exactly one log line for
the whole press, not a stream (that would be `LV_EVENT_PRESSING`, not
registered here).

## IMU

- **BMI270** (6-axis accelerometer + gyroscope). Supports interrupt-based
  wake-up, which confirms wake-on-motion from deep sleep is a real hardware
  capability, not just an assumption — see
  [power-management.md](power-management.md).

## RTC

- **RX8130CE**, with supercapacitor backup (70000µF/3.3V). Supports timed
  interrupt wake-up — this is a second confirmed wake source beyond touch/
  IMU, and one that doesn't require the Wi-Fi co-processor to be involved at
  all. Worth keeping in mind if a future revisit of the sleep model (see
  [power-management.md](power-management.md#notifications-during-sleeping))
  wants a lightweight periodic check-in without full reconnect cost.

## Power

- **Battery:** NP-F550 removable Li-ion, 7.4V @ 2000mAh (14.8Wh). Per
  [M5Stack's product page](https://docs.m5stack.com/en/core/Tab5), the
  Tab5 ships as two SKUs differing *only* in whether the battery is
  included: **K145** (with battery) and **C145** (without). The project's
  reference unit is the K145, but the battery is not a hard software
  requirement: a C145 unit should still run HomeDeck fully on USB-C power
  alone, just without portability. See [Battery-optional
  operation](#battery-optional-operation) below for what that means for
  the power state model.
- **Charging:** IP2326 charging management chip.
- **Conversion:** MP4560 buck-boost converter.
- **Power monitoring:** INA226 (I2C), providing real-time voltage *and*
  current monitoring. This is meaningfully better than raw ADC voltage
  sampling, but it is not a dedicated fuel-gauge/coulomb-counter IC — an
  accurate state-of-charge estimate still needs coulomb-counting or a
  voltage/current curve model implemented in firmware, not just reading
  INA226's instantaneous values directly as a percentage.
- **Wake-source aggregation:** a PMS150G-U06 interrupt controller aggregates
  wake interrupts (touch, IMU, RTC, power button) for the P4. This confirms
  the deep-sleep wake architecture described in
  [power-management.md](power-management.md) is real hardware, not
  speculative.

### Battery-optional operation

HomeDeck should run fully on a battery-less C145 unit powered from USB-C
alone — CLAUDE.md's "work fully using stock Tab5 hardware" requirement,
applied to both SKUs, not just the K145 reference unit. This is a real,
not hypothetical, gap in what's currently verified:

- **Unconfirmed:** what the INA226 power monitor (see [Power
  monitoring](#power) above) reports when no battery is physically
  attached — zero, a floating/garbage reading, or a dedicated
  presence-detection signal elsewhere in the charge circuit. `BatteryReader`
  needs to distinguish "no battery installed" from "battery installed,
  reading temporarily unavailable," not just report a raw percentage —
  otherwise a battery-less unit would show a permanently wrong or
  nonsensical battery indicator instead of correctly showing "no battery."
- **Not yet reflected in the design:** [power-management.md](power-management.md)'s
  explicit power states currently assume a battery is present and track
  its charge level; a "no battery, wired only" configuration isn't
  explicitly designed for yet — e.g. the Sleeping state's deep-sleep
  wake-cycle cost model exists specifically to conserve battery, which is
  moot with no battery to conserve.
- **Directly testable on the existing reference unit:** the battery is
  removable (NP-F550, clips onto the M-Bus connector — see
  [Physical form factor](#physical-form-factor) below), so this doesn't
  need separate base-Tab5 hardware to verify — unclip it from the K145 kit
  already in hand and confirm `BatteryReader`/the power state model behave
  correctly with it absent.

## Physical form factor

Confirmed against the project's own reference unit:

- The battery (see [Power](#power) above) clips onto the back of the
  device, on the opposite short edge from the USB ports.
- With the device held in the landscape orientation the current
  simulator/demo shell uses, the battery ends up on the right-hand side.
- The battery pack is ~2cm thick overall, but only ~0.6cm of that is
  recessed into the body — it protrudes ~1.4cm proud of the back. Laid
  flat, screen-up, on a surface, the device doesn't sit level — it tilts
  up at the battery end, acting as a passive kickstand without any
  dedicated stand hardware.
- The camera (see [Camera](#camera-out-of-current-scope) below, not used
  by HomeDeck) sits on the same right-hand edge as the battery in this
  orientation.

This is real input for the still-open landscape-vs-portrait orientation
decision, but doesn't resolve it by itself — recorded here as a confirmed
physical fact, independent of whatever that decision ends up being.

## Audio

- ES8388 codec, ES7210 AEC dual-mic array, 1W speaker, 3.5mm jack.

## Camera (out of current scope)

- SC2356 2MP, MIPI-CSI interface. Not part of CLAUDE.md's target hardware
  capability list — recorded here for completeness only. If a future use
  case wants it (e.g. a camera-based feature), this is available on the
  BOM; no current milestone plans to use it.
- **Considered and rejected** as the mechanism for Wi-Fi QR-code
  provisioning (see [networking.md](networking.md#initial-wi-fi-provisioning))
  — bringing up an isolated, currently-unverified MIPI-CSI camera driver
  plus a QR decode library for a single onboarding screen wasn't worth the
  risk versus SoftAP provisioning, which reuses infrastructure already
  committed to the project. Recorded here so this isn't silently
  re-proposed without the context of why it was passed over.

## microSD

Present on the BOM and explicitly named in CLAUDE.md's target hardware
capability list — unlike the items below, this is not out of scope. Used
for extended log archival (not backups, which are a Web UI download
instead, and not cached data, which lives on internal flash) — see
[core.md](core.md#responsibilities) and
[ADR-0012](../decisions/ADR-0012-storage-tiers.md#decision) for what's
stored where and why. Per CLAUDE.md's Target Hardware requirement that the
device "work fully using stock Tab5 hardware," and since a card is not
guaranteed to be present even when the slot is, microSD-backed features
must degrade gracefully when no card is inserted, not be required.

## Expansion (out of current scope)

RS-485, USB Type-A host, USB-C OTG, HY2.0-4P, M5-Bus, and STAMP pads
(Cat-M/NB-IoT/LoRaWAN cellular expansion). None of these are used by any
current milestone or named in CLAUDE.md's target hardware list; recorded
for completeness since they affect what "future hardware capability" could
mean without a rewrite.
