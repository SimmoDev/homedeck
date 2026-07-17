# Tab5 Hardware Reference

Concrete hardware facts about the M5Stack Tab5, compiled from M5Stack's
official documentation and product pages (2026-07). This exists because
several earlier architecture docs referenced "the Tab5's IMU," "the Tab5's
touch controller," etc. in the abstract without recording what they actually
are — decisions like JSON library choice (flash/RAM headroom) and
wake-from-sleep design were being made against assumptions rather than
verified facts.

Most of it has since been re-verified against the actual reference unit
during M1 bring-up — most sections below note what's been checked for
real (some with a bolded **Confirmed**/**Resolved** call-out, some in
plain prose) versus spec-sheet or datasheet claims not yet exercised.
Still worth updating here if anything drifts — M5Stack has already
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
- **Resolved:** the controller must be detected at runtime (`0x14` vs.
  `0x55`) rather than assumed via a compile-time flag — see
  [ADR-0009](../decisions/ADR-0009-touch-display-detection.md) for why a
  compile-time approach was rejected (it breaks the single-OTA-image
  model). ADR-0009's original plan was a hand-written I2C probe with a
  chip-ID register read to disambiguate ST7123 from ST7121 and a
  persisted result; that plan is superseded, not built — see the
  "Confirmed" bullet below and ADR-0009's own Consequences for why.
- **Confirmed:** the project's reference unit uses the **ST7123**
  integrated display+touch driver (I2C address 0x55) — read directly off
  the physical unit's sticker, no probing needed for this fact. The
  runtime detection itself is built and confirmed working too, just not
  via the hand-written design above — see [Display driver
  strategy](#display-driver-strategy) below: `espressif/m5stack_tab5`
  provides its own built-in probing between the two known hardware
  revisions, which is what actually covers detection across units in the
  field (a single reference unit knowing its own driver doesn't, by
  itself).
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

**Resolved: portrait, no rotation.** Reported resolution is `720x1280`
(portrait), not the `1280x720` spec-sheet figure — and this is genuinely
the panel's native scan direction, not just a default init flag: the BSP
hardcodes it as `BSP_LCD_H_RES`/`BSP_LCD_V_RES` in
`managed_components/espressif__m5stack_tab5/include/bsp/display.h`, with
no swap_xy applied at panel-init time. A 90° software rotation to
landscape is available via the BSP's `bsp_display_rotate()` (wrapping
LVGL's `lv_disp_set_rotation()`), but was rejected: the BSP's own source
flags that its anti-tearing mode isn't supported under software rotation,
and a different project (ESPHome) hit an open, unresolved bug combining
LVGL + 90° rotation on this same MIPI-DSI panel class — a blank screen
from a driver/LVGL dimension mismatch
([esphome/esphome#10740](https://github.com/esphome/esphome/issues/10740)),
corroborating real risk, not just a hypothetical one. Also weighed: the
battery pack's kickstand tilt (see [Physical form
factor](#physical-form-factor) below) lifts the device's top edge in
portrait — an easel angle facing the viewer — versus a sideways lean off
the right edge in landscape. `DashboardScreen`'s widgets needed no layout
changes either way, since both are placed with `LV_ALIGN_CENTER`/
`LV_ALIGN_TOP_RIGHT` relative to the parent, not fixed coordinates — only
the simulator's window resolution constants (`simulator/main.cpp`) and
its `UiTask`'s new desktop-only `zoom` parameter (SDL window size, not
LVGL's logical resolution, so a 1280px-tall canvas doesn't demand that
much vertical monitor space during development) changed. Firmware needed
no change at all — `homedeck.cpp` never called `bsp_display_rotate()`, so
it was already running the resolved orientation without knowing it.

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

## On-device dashboard

The real dashboard - `EventBus`, `Clock`, `DashboardScreen`, reused
directly from `src/`, not reimplemented for firmware - runs live on the
Tab5, confirmed via a live ticking clock and a real (not mocked) battery
percentage both actually sourced from hardware. This needed a real
`src/platform/firmware/` layer, none of which existed before:

- **`Task`/`Timer`** - FreeRTOS-backed, per ADR-0002's already-decided
  design (`xTaskCreate`/`xTimerCreate` directly, not built on
  `std::jthread` like the host backend, since ESP-IDF's C++ standard
  library threading support isn't needed here). Getting destruction
  ordering right needed real care: FreeRTOS has no built-in "join" for a
  self-deleting task, and `xTimerDelete` returning only means the delete
  *command* was queued, not that it's been processed - both destructors
  block on an explicit completion signal (a semaphore, and for `Timer` a
  pended function call ordered after the delete in FreeRTOS's single
  shared timer-command queue) rather than assuming either call is
  synchronous. A private nested `Impl` type (matching the host backend's
  pattern) can't be named from the free C function FreeRTOS's API
  requires as a callback, unlike a capturing lambda - both backends use a
  separate, ordinary (non-nested) context struct that `Impl` merely owns
  a pointer to, worked around rather than by loosening `Impl`'s access.
- **`BatteryReader`** via the INA226 (`espp/ina226`) and **`TimeSource`**
  via the RX8130CE RTC (`espp/rx8130ce`) - a third hardware support
  library, distinct from `espressif/m5stack_tab5`, since its capability
  table doesn't cover either peripheral - see
  [ADR-0016](../decisions/ADR-0016-battery-rtc-library.md) for why. See
  [Power](#power) and [RTC](#rtc) above for what those real reads
  actually showed. Both
  `espp` components communicate via function-pointer glue matching their
  `BasePeripheral` shape, not a bus handle directly - a small shared
  `I2cDevice` helper wraps ESP-IDF's `i2c_master` driver once and is
  reused by both, rather than duplicated. Both reuse the BSP's existing
  shared I2C bus (`bsp_i2c_get_handle()`) instead of creating a second,
  conflicting one on the same physical pins.

Three real gaps surfaced getting this to actually build and run, none of
them display/logic bugs:

- **ESP-IDF disables C++ RTTI by default.** `EventBus` uses
  `typeid()`/`std::type_index` for its per-event-type dispatch - a real,
  load-bearing design choice, not something to work around. Fixed via
  `CONFIG_COMPILER_CXX_RTTI=y` in `firmware/sdkconfig.defaults`.
- **The default single-app partition table (1024K) left only 1% free**
  once the real dashboard was actually linked in (RTTI, the BSP, LVGL,
  and the new platform code all add up). Fixed with ESP-IDF's built-in
  larger single-app table (1500K,
  `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`) as a pragmatic unblock -
  not the real OTA A/B partition table, which stays explicit M2 scope
  per ADR-0012 and the roadmap's OTA item. Confirmed via a real build:
  32% free on the 1500K table with the full M1 dashboard linked in
  (`espp/ina226`/`espp/rx8130ce` included, per
  [ADR-0016](../decisions/ADR-0016-battery-rtc-library.md)). Expect to
  need more headroom again as more gets built.
- **The Docker build only had `firmware/` visible, not the repo root** -
  fine while firmware only contained its own template code, but this
  step needed `../../src` (the reused portable source), which lives
  outside that mount entirely. Fixed by mounting the whole repo root and
  adjusting the working directory instead (`-v "$(pwd):/project" -w
  /project/firmware`) - see
  [DEVELOPMENT.md](../../DEVELOPMENT.md#esp-idf-setup) for the current
  commands; every previously-documented flash/monitor command changed
  because of this.

`firmware/main/CMakeLists.txt` lists the reused `src/` files directly by
relative path rather than nesting `src/CMakeLists.txt`'s plain-CMake
`add_subdirectory` build inside this ESP-IDF component - the two build
systems have different conventions, and integrating them properly
(confirming LVGL's `TARGET lvgl` check in `src/CMakeLists.txt` resolves
correctly against the managed `lvgl` component, in particular) is real,
unverified risk not taken on for this step. A known tradeoff: these paths
need updating by hand if `src/`'s own file list changes.

Deliberately out of scope for this step: Navigation, the home affordance,
and any second screen - the dashboard is loaded directly as the only
screen. `Queue<T>`'s firmware backend also stays deferred - still nothing
in this codebase uses it.

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
- **Never been set:** the reference unit's RTC reads a meaningless
  factory/power-on date (confirmed on hardware via
  [On-device dashboard](#on-device-dashboard) below reading real RTC
  time for the first time) - expected, not a fault. No time-setting
  mechanism exists yet; needs either SNTP over Wi-Fi or a manual
  set-time affordance, both M2 scope (networking and Web/Touch UI
  respectively).

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
  INA226's instantaneous values directly as a percentage. **Confirmed on
  hardware:** the simple linear approximation `Ina226BatteryReader`
  actually uses (see [On-device dashboard](#on-device-dashboard) below)
  reads ~90%, not 100%, on a pack that had been on USB power long enough
  to be fully charged. A real state-of-charge estimate is still M2
  power-management scope, not fixed here.
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
- Held in the portrait orientation the software now uses (see [Display
  driver strategy](#display-driver-strategy) above), the battery sits at
  the **top** edge, USB ports at the bottom — the natural grip for a
  handheld remote, cable/charging access at the bottom like a phone.
- The battery pack is ~2cm thick overall, but only ~0.6cm of that is
  recessed into the body — it protrudes ~1.4cm proud of the back. Laid
  flat, screen-up, on a surface, the device doesn't sit level — it tilts
  up at the battery end. In this orientation that's the top edge, so the
  tilt works as a passive kickstand: an easel angle facing the viewer,
  not a sideways lean — real physical input that fed the portrait
  decision, not just a coincidence noted after the fact.
- The camera (see [Camera](#camera-out-of-current-scope) below, not used
  by HomeDeck) sits on the same top edge as the battery in this
  orientation.

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
