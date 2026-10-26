# ADR-0033: Restarts Reset the Whole System

## Status

Accepted.

## Context

`esp_restart()` on the ESP32-P4 resets the CPUs and a fixed set of
peripherals (DMA engines, timers, UARTs, crypto) and leaves the rest, including
the SDMMC host and the SDIO pads, as they were. ESP-Hosted's SDIO link to the
ESP32-C6 Wi-Fi co-processor then fails after a run of such restarts: SDIO
commands time out, reads return all ones, and D1 (the slave interrupt line)
reads low. Every following boot fails the same way until a whole-system reset.
Neither a CPU-only restart, nor cycling the C6's power rail, nor
ESP-Hosted's own GPIO reset of the C6 clears it, so the stuck state is not in
the C6. ESP-Hosted restarts the host itself when the transport fails
(`ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE`, enabled by default), so a failed
boot restarts into the same failure.

A reboot loop on the reference unit failed within 35 to 75 boots in most runs
with CPU-only restarts, and ended every time with a whole-system reset booting
healthily. Roughly 290 consecutive boots with whole-system resets showed no
SDIO failure.

## Decision

`firmware/main/homedeck.cpp` registers a shutdown handler,
`esp_register_shutdown_handler(ResetWholeSystem)`, that calls
`esp_rom_software_reset_system()`. `esp_restart()` runs the registered
handlers before it resets anything, so every restart - the Web UI's reboot and
Wi-Fi-reset actions and ESP-Hosted's transport-failure restart alike - ends in
a whole-system reset. It is registered first, so it runs after any handler
registered later.

**Rejected - handling the transport-failure event in HomeDeck.** It would
cover only ESP-Hosted's restart and leave HomeDeck's own restarts with the same
fault.

**Rejected - cycling the C6's power rail on every boot.** It did not clear the
fault and added a delay to every boot.

## Consequences

- The reset reason after a restart is still reported as `software`.
- `esp_restart()`'s own steps (a PSRAM data-cache write-back and the
  peripheral resets) are skipped, which does not matter since the whole
  system, including those peripherals, is reset immediately.
- Shutdown handlers run in reverse registration order, so the handler runs
  before the ones ESP-IDF registers during startup, before `app_main()`.
  Those are skipped; the only one in this configuration is
  `esp_sync_timekeeping_timers`, which carries the system time across a CPU
  restart. HomeDeck sets the time from the RX8130 RTC and SNTP at boot.
- Restarts that do not go through `esp_restart()`, such as a panic's, are
  unchanged.
- The Wi-Fi-connect panic recorded in [hardware.md](../architecture/hardware.md#wi-fi-bring-up)
  is a separate, unresolved issue.
