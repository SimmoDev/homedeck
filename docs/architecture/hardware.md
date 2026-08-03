# Tab5 Hardware Reference

Concrete hardware facts about the M5Stack Tab5, compiled from M5Stack's
official documentation and product pages plus the project's own reference
unit (2026-07). Sections below distinguish what's been checked against
the physical reference unit (marked **Confirmed**) from spec-sheet or
datasheet claims not yet exercised.

M5Stack has revised this hardware twice during its production life (see
[Display and touch](#display-and-touch) below) — treat "confirmed
2026-07" as a snapshot, not a permanent guarantee, and update this file
if a fact drifts.

## Application processor

- **ESP32-P4**, dual-core RISC-V, 400MHz. No integrated radio (Wi-Fi/BT) —
  see [Wireless](#wireless) below.
- 16MB Flash, 32MB PSRAM. The ESP32-P4 has no Quad/Octal PSRAM choice
  (unlike other Espressif chips) — its only mode is what ESP-IDF's own
  Kconfig and boot log both call "Hex" (16-line), a P4-specific memory
  controller mode. `firmware/sdkconfig.defaults` reflects this (just
  `CONFIG_SPIRAM=y`, no mode override needed).
- **Confirmed** (see [roadmap.md](../roadmap.md)'s M1 Tab5 boot item, and
  `firmware/sdkconfig.defaults`): chip revision v1.3, 16MB flash
  physically detected, 32MB PSRAM detected and initialized (vendor "AP",
  generation 4) with ~33.5MB reported free. Boots at 360MHz by default
  (400MHz is the rated max, not necessarily the default without explicit
  clock config — not changed, since nothing currently needs the higher
  clock).
- **PSRAM runs at 200MHz** (`sdkconfig.defaults`), not Kconfig's 20MHz
  default — 20MHz causes a continuous DMA underrun in the display's
  PSRAM-backed framebuffer path (see
  [Display and touch](#display-and-touch) below). These are the *only*
  two speeds ESP-IDF exposes for this chip's PSRAM controller — no
  100MHz or other intermediate option exists despite what the Kconfig
  help text implies. 200MHz requires
  `CONFIG_IDF_EXPERIMENTAL_FEATURES=y`, ESP-IDF's own marker that this
  isn't a fully hardened path — worth remembering if anything unrelated
  gets flaky later.

## Wireless

- **ESP32-C6-MINI-1U**, acting as a wireless co-processor for the P4 (Wi-Fi
  6, BLE, Thread, Zigbee). This confirms the dual-chip architecture flagged
  as a risk during architecture review: Wi-Fi is not native to the
  application processor, it runs through a companion chip over a host-slave
  link. See [networking.md](networking.md) and
  [power-management.md](power-management.md) for the consequences (reconnect
  cost on wake, OTA scope question for the co-processor's own firmware).
- **Link: SDIO**, pins `SDIO2_CLK` = GPIO 12, `SDIO2_CMD` = GPIO 13,
  `SDIO2_D0`–`D3` = GPIO 11/10/9/8, slave reset = GPIO 15 — set via
  `CONFIG_ESP_HOSTED_SDIO_SLOT_1=y`, the individual
  `CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_*_SLOT_1` options, and
  `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE` in `sdkconfig.defaults`.
  ESP-Hosted's Kconfig defaults assume Espressif's P4 eval board wiring,
  which doesn't match this hardware. Note: GPIOs 43/44/39/40/41/42 are a
  *separate* pin group entirely — the microSD card slot's own SDMMC host
  instance, not the C6's SDIO link — easy to confuse since the numbers
  fall in the same range.
- **Power domain confirmed.** The C6's power enable (`WLAN_PWR_EN`) is
  bit 0 of a dedicated I2C GPIO expander output (PI4IOE5V6408, I2C
  address `0x44` — see the address map above), toggled via
  `bsp_feature_enable(BSP_FEATURE_WIFI, true)` — not
  `bsp_set_wifi_power_enable()`, which is a different repo's (M5Stack's
  own `M5Tab5-UserDemo`) function name and doesn't exist in this
  project's actual dependency (`espressif/m5stack_tab5`). Confirmed
  necessary together with the correct SDIO pins above; not verified in
  isolation. No automatic hardware coupling to the P4's own sleep state
  exists in that BSP source — the C6's power rail is independently
  switchable, not wired to collapse whenever the P4 sleeps. This answers
  the "are they wired together" question the alert-priority wake cycle's
  original design once cared about — see
  [ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md) for why
  that design no longer applies, which makes whether the C6 can stay
  associated through P4 deep sleep moot for `Sleeping` (P4 never sleeps
  in the redefined design). Whether it would still matter for a possible
  future full board power-off feature depends on what `VDD_STBY` on the
  PMS150G-U06 actually powers, which isn't confirmed — see [Wake
  sources](#power) under Power above; that feature would need its own
  investigation, not an inherited assumption from this paragraph.

### Wi-Fi bring-up

A real Wi-Fi connection over the C6, via ESP-Hosted, is confirmed working
end to end on hardware — a real IP address from a real access point, not
just the SDIO link coming up. Required configuration beyond the SDIO
pins and power enable above:

- `CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y` — ESP-Hosted's SDIO
  transport needs a ~48KB contiguous block of internal DMA-capable RAM
  that isn't reliably available that early in boot; this redirects those
  buffers into PSRAM, the same GDMA-through-PSRAM-cache path the display
  already relies on.
- The `esp_wifi_remote`/`esp_hosted` dependency resolves to the
  `esp_hosted` `2.12.x` line, not `3.0.0`, which requires `idf >=5.5` and
  would conflict with this project's `v5.4.3` pin (ADR-0014).

**Known follow-up, not blocking:** the connection log includes `Version
mismatch: Host [2.12.0] > Co-proc [0.0.0] ==> Upgrade co-proc to avoid RPC
timeouts` — the C6's own ESP-Hosted slave firmware reports version
`0.0.0`. Basic Wi-Fi association and IP acquisition work regardless, but
RPC timeouts under heavier use remain a real, deferred risk.

**Known, not yet root-caused: an intermittent crash during the Wi-Fi-connect
burst.** Twice out of four on-device connection attempts, the device
panicked (`Guru Meditation Error: Instruction access fault`, Core 1) a few
hundred milliseconds after logging a successful IP acquisition, during the
highest-SDIO-traffic window in the boot sequence. Root-cause investigation
has exhausted every avenue available without JTAG hardware debug tooling
or upstream engagement with the `esp-hosted-mcu` project; the fault
signature doesn't match any currently-open upstream issue for this exact
ESP32-P4+C6 combination. See git history for the diagnostic detail behind
this conclusion.

Reproducing this repeatedly no longer needs a full
`tools/factory-reset.sh` erase-and-reflash cycle per attempt - the Web
UI's Diagnostics page has a **Reset Wi-Fi credentials** action
(`POST /api/wifi/reset`, see [web-ui.md](web-ui.md#diagnostics)) that
clears just the stored Wi-Fi credentials and reboots automatically so
the device re-enters SoftAP setup, isolating the connect burst without
touching Core's own `Storage` state.

**Accepted risk:** carried forward into M3 as a known, explicitly
accepted gap, not resolved. The device recovers on its own (a
self-triggered reboot, not a hang). Accepted specifically because every
root-cause avenue available without JTAG or upstream engagement has
already been exhausted, not because the reproduction rate is considered
acceptable in the abstract - re-evaluate once M3 adds more Wi-Fi-adjacent
activity at boot/connect time (Harmony hub discovery, etc.), which could
plausibly change that rate one way or the other.

The real provisioning flow (`firmware/main/wifi_setup.cpp`) is a SoftAP +
minimal HTTP setup form, not ESP-IDF's `wifi_provisioning` component —
see [ADR-0026](../decisions/ADR-0026-wifi-provisioning-mechanism.md)
for why. SoftAP setup, credential submission, connection, and SoftAP
teardown all work end to end, including with a non-alphanumeric SSID
(an apostrophe). The form's submitted values are
percent-decoded and length-validated before being applied
(`src/core/url_codec.h`, `src/core/wifi_credentials.h`) so a network name
or password containing a space or symbol is handled correctly rather than
corrupted.

Two standing facts about this flow:
- **Wi-Fi credentials live on the C6 co-processor's own flash, not the
  P4's `nvs` partition** — `esp_wifi_get_config`/`esp_wifi_set_config`
  are RPC calls proxied to the C6 via `esp_wifi_remote`, and the C6
  persists them itself. `esp_wifi_restore()` (also proxied), not erasing
  the P4's `nvs` region, is the correct way to clear them. Moving
  credential storage onto Core's own service (a known gap, see
  [ADR-0026](../decisions/ADR-0026-wifi-provisioning-mechanism.md#consequences))
  will need `esp_wifi`'s storage mode set to `WIFI_STORAGE_RAM` so the
  co-processor stops persisting it a second time.
- **`esp_http_server`'s max request header size is raised to 4096 bytes**
  (`CONFIG_HTTPD_MAX_REQ_HDR_LEN`) — the 512-byte default is too small
  for a real mobile browser's POST to the setup form. A RAM buffer, not
  flash, so it doesn't compete with the headroom below.

Flash headroom with `esp_wifi_remote`, `esp_hosted`, `esp_http_server`,
and their dependencies (`wpa_supplicant`, `mbedtls`, etc.) linked in is
tight on a single-app partition table — the real OTA A/B table (see
[ADR-0017](../decisions/ADR-0017-partition-table.md)) provides real
headroom instead, 4MB each for `ota_0`/`ota_1`.

## Display and touch

- 5" 1280×720 IPS, MIPI-DSI interface.
- **Known hardware revision fragmentation:** the original display/touch
  driver pairing was ILI9881C (display) + GT911 (touch controller, I2C
  address 0x14). M5Stack replaced this with an integrated ST7123
  display+touch driver (I2C address 0x55) starting 2025-10-14, then again
  with ST7121 (also 0x55) starting 2026-04-28. Units purchased at different
  times may run different silicon here, and testing on one unit does not
  validate behavior on all of them.
- The controller is detected at runtime (`0x14` vs. `0x55`) rather than
  assumed via a compile-time flag — see
  [ADR-0009](../decisions/ADR-0009-touch-display-detection.md) for why a
  compile-time approach was rejected (it breaks the single-OTA-image
  model), and the "Confirmed" bullet below for how detection is
  actually implemented.
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
use it outside the Arduino IDE, which [CLAUDE.md](../../CLAUDE.md) rules out) —
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
merge time, with limited field validation.

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

**Confirmed:** a solid color fill displays correctly on the physical
panel, with no PSRAM-DMA underrun (see [Application
processor](#application-processor) above for the required PSRAM speed).
`espressif/m5stack_tab5`'s runtime probing detects "board version 2 (LCD
ST7123, Touch ST7123)", matching the physical sticker. Touch initializes
successfully (10-point multitouch).

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

**Confirmed:** runs live on the Tab5, not just the simulator - a real
ticking clock and a real (not mocked) battery percentage, both sourced
directly from hardware via `BatteryReader`
(`src/platform/firmware/battery_reader.h`) reading the INA226 (see
[Power](#power) above) and `TimeSource`
(`src/platform/firmware/time_source.h`) reading the RX8130CE RTC (see
[RTC](#rtc) above) - a third hardware support library (`espp`), distinct
from `espressif/m5stack_tab5`, since its capability table doesn't cover
either peripheral (see
[ADR-0016](../decisions/ADR-0016-battery-rtc-library.md)). Both reuse
the BSP's existing I2C bus (`bsp_i2c_get_handle()`) rather than a
second, conflicting one on the same physical pins.

## IMU

- **BMI270** (6-axis accelerometer + gyroscope). The chip itself supports
  interrupt-based wake-up, but that's a datasheet capability, not a
  confirmation about this board's wiring — its `INT1`/`INT2` pins have no
  confirmed path to the P4, see [Wake sources](#power) under Power below.

## RTC

- **RX8130CE**, with supercapacitor backup (70000µF/3.3V). The chip itself
  supports timed interrupt wake-up, but that's a datasheet capability, not a
  confirmation about this board's wiring — its `nIRQ` pin has no confirmed
  path to the P4, see [Wake sources](#power) under Power below.
- **Corrected via SNTP once Wi-Fi (and internet reachability) is
  available** — see [ADR-0028](../decisions/ADR-0028-time-synchronization.md)
  for the full design. Before this, and on a device with no internet
  route, the RTC reads whatever it last held — a meaningless factory/
  power-on date on a never-synced unit, degrading gracefully rather than
  failing loudly, consistent with this project's offline-behavior
  philosophy. Not corrected to the user's actual local timezone — the
  RTC's raw fields are interpreted with no timezone math anywhere in this
  project (see [On-device dashboard](#on-device-dashboard) below and
  ADR-0028's own Consequences), a known, separate gap.

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
- **Charging:** IP2326 charging management chip, controlled through the
  same PI4IOE5V6408 IO expander at I2C `0x44` used for Wi-Fi power
  enable (see [Wireless](#wireless) above) - this board has two such
  expanders (`0x43` and `0x44`), confirmed against M5Stack's official
  [Tab5 pinmap](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/C145_Pinmap_Overview.png),
  which labels them `E1`/`E2` respectively. On `E2` (`0x44`), `P7` is
  `CHG_EN`: not automatic - the enable line needs its high-Z
  (open-drain) bit cleared as well as its output level set, or it stays
  electrically floating rather than actually driving. **Confirmed:**
  battery percentage climbs normally under USB-C power once both are set.
- **Charge status:** `E2.P6` is `CHG_STAT`, also confirmed against the
  official pinmap - high only while the IP2326 is actively driving
  charge current into a battery, low once charging terminates (e.g. a
  full battery) even with USB-C still connected. There is no separate
  raw cable/VBUS-presence pin exposed via either IO expander on the
  pinmap, confirmed both by tracing every USB-C connector net on the
  full schematic and against M5Stack's own M5Unified library (Tab5's
  `isCharging()` reads this identical bit). Needs a pull-down
  explicitly enabled (not left floating) to read cleanly - confirmed on
  hardware, matching M5Stack's own
  [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo)
  reference firmware, which enables one on this exact pin.
- **No battery-temperature signal exists, at any level - confirmed
  against the schematic.** `U20` (the IP2326)'s `NTC` pin (pin 4) is
  not fed by a thermistor: it's tied to a fixed/switched resistor
  divider (`R66`/`R68`/`R69`, all 49.9K/1%) between `SOC_3.3V` and
  `GND`, gated by `Q1` under `nCHG_QC_EN` - a quick-charge-negotiation
  signal, unrelated to temperature - so the IC's own built-in thermal
  protection is permanently spoofed to "normal," not merely unused.
  `J12` (`CON4_SMD`, the battery pack connector) is 4 pins + shield:
  pin 1 is `SYS_BAT_PRE` (through `Q5`), pins 3/4/shield are `GND`, and
  pin 2 - the only candidate for a pack-side thermistor line - is
  unconnected on the board side.
- **Battery presence:** current is the primary signal for whether a
  battery is physically installed. **Confirmed:** bus voltage alone
  cannot tell "no battery" apart from "battery present" - with charging
  enabled and nothing connected to charge, the IP2326
  hunts for its regulation target on the unloaded output, swinging
  between roughly 4V and the 100%-mapped voltage (8.4V, see below)
  every tick rather than settling. Current reads a flat 0.000000A with
  no battery, and settles to a small but clearly nonzero, stable value
  within one tick of a real pack connecting - but current alone also
  can't distinguish "no battery" from "battery present, full, charging
  terminated," since both read that same flat zero. Voltage remains
  usable there: an installed battery holds the rail steady via its own
  chemistry (consecutive readings stay within tens of mV even right at
  the charging-terminates transition), unlike the
  multi-volt hunting swing with no battery at all.
- **Conversion:** MP4560 buck-boost converter.
- **Power monitoring:** INA226 (I2C), providing real-time voltage *and*
  current monitoring. This is meaningfully better than raw ADC voltage
  sampling, but it is not a dedicated fuel-gauge/coulomb-counter IC — an
  accurate state-of-charge estimate still needs coulomb-counting or a
  voltage/current curve model implemented in firmware, not just reading
  INA226's instantaneous values directly as a percentage. **Confirmed:**
  the simple linear approximation this project uses today
  (see [On-device dashboard](#on-device-dashboard) below) reads ~90%,
  not 100%, on a pack that had been on USB power long enough to be
  fully charged. A real state-of-charge estimate is still M2
  power-management scope, not fixed here.
- **Wake sources - no confirmed GPIO path for touch/IMU/RTC:** traced
  directly against M5Stack's official schematic
  ([`Tab5_Schematics_PDF.pdf`](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf),
  linked from the [product page](https://docs.m5stack.com/en/core/Tab5),
  same source as the pinmap above). **PMS150G-U06 (U28) is a
  power-button/status-LED controller, not a wake aggregator** - pin 1
  is `SW_PWR` (the physical power button), pin 5 is `LED_GREEN`, pin 6
  is `VDD_STBY`, and pin 4 is its only connection to the P4: net
  `BOOT_GPIO35` (confirmed by the same net name at the P4's own GPIO35
  pin elsewhere in the schematic). **Touch's `TP_INT` connects directly
  to the P4 at GPIO23**, bypassing PMS150 entirely - confirmed twice in
  the schematic and matching
  `firmware/components/m5stack_tab5/include/bsp/m5stack_tab5.h`'s
  `BSP_LCD_TOUCH_INT` (`GPIO_NUM_23`). **BMI270's `INT1`/`INT2` pins and
  the RX8130's `nIRQ` pin each appear with no net name reaching
  anywhere else in the document** - every genuinely-wired net in this
  schematic (e.g. `BOOT_GPIO35`, `TP_INT_GPIO23`) shows up at both its
  source and destination; these don't, which is real but not certain
  evidence (noisy PDF-text extraction, not an explicit "NC" label) that
  they're unconnected on this board. **Both P4-side pins actually found
  (GPIO35, GPIO23) sit outside GPIO0-15** - confirmed against this
  project's own generated `sdkconfig.h` for the `esp32p4` target:
  `esp_sleep_enable_ext1_wakeup_io()`/`esp_deep_sleep_enable_gpio_wakeup()`
  both require the wake pin to be in the RTC-IO domain
  (`SOC_RTCIO_PIN_COUNT=16`, i.e. GPIO0-15), which neither pin is. Net
  effect: as currently understood, none of touch, IMU, or RTC have a
  confirmed path to a P4 GPIO capable of waking the device from deep
  sleep via the standard ESP-IDF wake APIs. M5Stack's own official Tab5
  firmware doesn't route around this with wiring we missed - it doesn't
  use these APIs either, for the same three sources - see
  [ADR-0024](../decisions/ADR-0024-sleeping-wake-mechanism.md) for what
  it does instead and what that means for this project's design.

### Battery-optional operation

HomeDeck should run fully on a battery-less C145 unit powered from USB-C
alone — [CLAUDE.md](../../CLAUDE.md)'s "work fully using stock Tab5 hardware" requirement,
applied to both SKUs, not just the K145 reference unit. This is a real,
not hypothetical, gap in what's currently verified:

- **Confirmed:** bus-voltage readings are not meaningful with no battery
  attached - they swing unpredictably (see [Power](#power)'s Battery
  presence bullet), never settling. Current-based detection is the real
  "no battery installed" vs. "battery installed" signal on the K145
  reference unit. A running C145 unit has no possible power source
  other than USB-C. See
  [dashboard.md](dashboard.md#status) for how the Web/Touch UI uses
  this.
- **Not yet reflected in the design:** [power-management.md](power-management.md)'s
  explicit power states currently assume a battery is present and track
  its charge level; a "no battery, wired only" configuration isn't
  explicitly designed for yet — e.g. the Sleeping state's deep-sleep
  wake-cycle cost model exists specifically to conserve battery, which is
  moot with no battery to conserve.

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
  not a sideways lean.
- The camera (see [Camera](#camera-out-of-current-scope) below, not used
  by HomeDeck) sits on the same top edge as the battery in this
  orientation.

## Audio

- ES8388 codec, ES7210 AEC dual-mic array, 1W speaker, 3.5mm jack.
- I2S pins: MCLK=GPIO30, SCLK=GPIO27, LCLK=GPIO29, DOUT=GPIO26,
  DSIN=GPIO28.
- ES8388 at I2C address 0x10 (see the address map above), ES7210 at 0x40.
- The speaker amp is gated by an output pin on the same I2C GPIO
  expander as the LCD/touch/camera enables (not the Wi-Fi/USB one) —
  not by a direct GPIO, which is unconnected on this board.
- **Confirmed:** enabling that output pin enables the amp (audible
  output at full digital volume on the K145 reference unit's speaker).

## Camera (out of current scope)

- SC2356 2MP, MIPI-CSI interface. Not part of [CLAUDE.md](../../CLAUDE.md)'s target hardware
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

Present on the BOM and explicitly named in [CLAUDE.md](../../CLAUDE.md)'s target hardware
capability list — unlike the items below, this is not out of scope. Used
for extended log archival (not backups, which are a Web UI download
instead, and not cached data, which lives on internal flash) — see
[core.md](core.md#responsibilities) and
[ADR-0012](../decisions/ADR-0012-storage-tiers.md#decision) for what's
stored where and why. Per [CLAUDE.md](../../CLAUDE.md)'s Target Hardware requirement that the
device "work fully using stock Tab5 hardware," and since a card is not
guaranteed to be present even when the slot is, microSD-backed features
must degrade gracefully when no card is inserted, not be required.

## Expansion (out of current scope)

RS-485, USB Type-A host, USB-C OTG, HY2.0-4P, M5-Bus, and STAMP pads
(Cat-M/NB-IoT/LoRaWAN cellular expansion). None of these are used by any
current milestone or named in [CLAUDE.md](../../CLAUDE.md)'s target hardware list; recorded
for completeness since they affect what "future hardware capability" could
mean without a rewrite.
