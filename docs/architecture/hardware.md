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
- 16MB Flash, 32MB PSRAM (Octal).

## Wireless

- **ESP32-C6-MINI-1U**, acting as a wireless co-processor for the P4 (Wi-Fi
  6, BLE, Thread, Zigbee). This confirms the dual-chip architecture flagged
  as a risk during architecture review: Wi-Fi is not native to the
  application processor, it runs through a companion chip over a host-slave
  link. See [networking.md](networking.md) and
  [power-management.md](power-management.md) for the consequences (reconnect
  cost on wake, OTA scope question for the co-processor's own firmware).
- **Not yet confirmed:** whether the C6's power/SDIO domain is independent
  of the P4's deep-sleep domain. If independent, the C6 could stay in
  Wi-Fi modem-sleep (associated, low duty cycle) through P4 deep sleep
  instead of losing its association entirely — a materially different, and
  cheaper, wake-cycle cost model than full re-association. This changes
  the *shape* of the alert-priority wake cycle's cost, not just its tuned
  numbers — see
  [power-management.md](power-management.md#notifications-during-sleeping).
  Confirm during M1 against the actual board/schematic, not assumed either
  way.

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
  ST7123/ST7121 need genuinely separate driver paths). Confirm which
  revision M5GFX/M5Unified's existing Tab5 support already targets before
  writing new detection code, and record which revision the project's own
  reference hardware uses.

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

- **Battery:** NP-F550 removable Li-ion, 7.4V @ 2000mAh (14.8Wh). **Only
  included with the K145 kit variant** — the base Tab5 may ship without a
  battery. HomeDeck's "battery-powered handheld" identity depends on this
  being the reference SKU; confirm which exact kit/SKU is the project's
  reference hardware and record it here.
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
