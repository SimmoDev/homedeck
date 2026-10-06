# ADR-0031: Internal RAM Budget and `malloc()` Placement

## Status

Accepted

## Context

The ESP32-P4 has roughly 250 KB of internal RAM available to the
application after display initialisation, and 32 MB of PSRAM
([hardware.md](../architecture/hardware.md)). With
`CONFIG_SPIRAM_USE_MALLOC=y`, `malloc()` tries internal RAM first for
any request up to `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` bytes and falls
back to PSRAM only when that fails. At the default of 16384 every LVGL
object, string and JSON node is a small request, so building the UI took
about 125 KB of internal RAM and the running system settled at 4-30 KB
free.

Some allocations can only come from internal RAM, notably every
FreeRTOS task stack. `esp_websocket_client` creates a task with a 4 KB
stack on every `Connect()`, so a Harmony or Kodi reconnect with no 4 KB
internal block left fails with `Error create websocket task` and the
module stays disconnected.

## Decision

`firmware/sdkconfig.defaults` sets `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`
so every plain `malloc()` prefers PSRAM, and
`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536` so the last 64 KB of
internal RAM is never consumed by PSRAM-capable `malloc()`, even as a
fallback. Internal RAM is left for allocations that name it explicitly:
task stacks, DMA buffers and the Wi-Fi/lwIP stacks.

The firmware logs internal free, largest free block and low-water mark
every 30 seconds ([diagnostics.md](../architecture/diagnostics.md#memory)).

**Rejected - routing only LVGL's allocator to PSRAM.** It would leave
strings, JSON and every other module allocation competing for the same
pool; the global setting covers all of them with no per-call-site
discipline.

## Consequences

- Plain-`malloc()` data (LVGL objects, JSON, strings) is accessed over
  the PSRAM bus, which has higher latency than internal RAM. The UI is
  object bookkeeping, not per-frame-critical; scroll performance of very
  large lists is the case to watch.
- Code that needs internal memory (an ISR-visible buffer, a DMA
  descriptor) must say so with `heap_caps_malloc(..., MALLOC_CAP_INTERNAL)`;
  plain `malloc()` no longer provides it.
- Steady-state internal free is about 140 KB with Wi-Fi, Harmony and
  Kodi connected. A new task or per-connection allocation should be
  checked against that figure.
