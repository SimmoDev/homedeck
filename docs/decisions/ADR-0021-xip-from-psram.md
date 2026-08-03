# ADR-0021: Execute-in-Place from PSRAM to Eliminate the Flash-Write Display Glitch

## Status

Accepted

## Context

[ADR-0020](ADR-0020-async-log-persistence.md) reduced the flash-write
display glitch (a brief, uniform color-shifted tint over otherwise-correct
content) from three occurrences per boot to one, but left a single
`Logger::Log()` write - or any other flash write, at any point during
active use - as a still-open source of the same corruption, with no
available fix identified at the time.

Further investigation identified the exact mechanism from ESP-IDF v5.4.3
source rather than inference. A SPI flash program/erase operation calls
`spi_flash_disable_interrupts_caches_and_other_cpu()`
(`components/spi_flash/cache_utils.c`), which disables the ESP32-P4's L2
cache for the operation's duration. That cache sits in front of *both*
flash and PSRAM (`CACHE_LL_EXT_MEM_VIA_L2CACHE=1`,
`hal/esp32p4/include/hal/cache_ll.h`), so disabling it also blocks the
DSI panel driver's DMA engine from reading the PSRAM-resident frame
buffer for the operation's duration - starving the display mid-scanout.
ESP-IDF's own DSI DPI driver (`components/esp_lcd/dsi/esp_lcd_panel_dpi.c`)
detects exactly this condition (`MIPI_DSI_LL_EVENT_UNDERRUN`) and logs
it with a comment matching the observed symptom precisely: *"when an
underrun happens, the LCD display may already becomes blue... optimize
the memory bandwidth (with AXI-ICM)."*

ESP-IDF documents a built-in escape hatch for this exact mechanism:
`CONFIG_SPIRAM_XIP_FROM_PSRAM`
(`docs/en/api-guides/external-ram.rst`). With `.text`/`.rodata` moved
into PSRAM, code execution no longer depends on the cache being enabled,
so ESP-IDF skips disabling it for a flash operation entirely - not a
workaround for the stall, but removal of its cause. The same
documentation names the trade-off: since the ESP32-P4's flash and PSRAM
are on separate SPI buses, moving code into PSRAM increases steady-state
PSRAM bus load (every instruction fetch now competes with DSI scanout,
not just flash writes), so the net effect on an already PSRAM-bandwidth-
sensitive display path isn't guaranteed favorable without testing.

## Decision

**`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`** (`firmware/sdkconfig.defaults`).

20 consecutive hardware resets (EN-pin resets, not soft
`esp_restart()`), each completing a full boot through Wi-Fi connect and
the async Logger's batched flash write, produce zero DSI underrun
interrupts and zero visible display corruption. The glitch ADR-0020
could not fully resolve is gone.

## Consequences

- [ADR-0020](ADR-0020-async-log-persistence.md)'s async/batched
  persistence design is unchanged and still correct - coalescing writes
  remains good practice independent of this fix - but its "known
  hardware/BSP limitation with no complete fix available" conclusion no
  longer holds; that limitation is resolved by this decision.
- [hardware.md](../architecture/hardware.md)'s "known display glitch"
  entry is removed rather than amended - it described a since-fixed
  behavior, not a standing hardware characteristic.
- Steady-state PSRAM bus load is higher than before (per the trade-off
  above) - worth watching if a future PSRAM-bandwidth-sensitive feature
  (e.g. video/camera work) shows contention, though normal operation
  shows no regression from it today.
- microSD-backed log archival ([ADR-0012](ADR-0012-storage-tiers.md))
  remains open but is no longer motivated by this glitch - it stands or
  falls on its original "extended retention past the internal tier's
  bounded rotation" rationale alone.
