# ADR-0020: Asynchronous, Coalesced Log Persistence

## Status

Accepted — partially superseded by
[ADR-0021](ADR-0021-xip-from-psram.md), which resolves the single-write
display-glitch case this decision's Consequences section says has "no
safe, available fix." This ADR's own async/batched design is otherwise
unchanged.

## Context

[ADR-0019](ADR-0019-structured-logging.md) decided `Logger::Log()`
performs a synchronous whole-file `CacheStore::Write()` on every call.
On the K145 reference unit, three `Log()` calls landing within the same
second during boot (Wi-Fi connect, mDNS advertise, Web UI start)
reliably produce a visible display corruption - a brief, uniform
color-shifted tint over otherwise-correctly-rendered content. Removing
those three calls entirely eliminates the glitch, isolating the cause to
the flash write itself, not anything else in that boot window.

**Root cause:** the display framebuffer is PSRAM-resident
(`espressif/m5stack_tab5`'s DPI panel driver allocates it via
`MALLOC_CAP_SPIRAM`), and this unit's flash chip isn't vendor-identified
by ESP-IDF (`spi_flash: detected chip: generic`), so
`CONFIG_SPI_FLASH_AUTO_SUSPEND` isn't a safe option - a flash
program/erase operation can't be interrupted to service other bus/cache
access while in progress. NOR flash also requires a full sector erase
before reprogramming a previously-written region regardless of the
logical byte count changed, so even a small log entry write carries the
same worst-case duration as a large one. The exact mechanism by which
this disrupts the DSI DMA engine's framebuffer read isn't independently
confirmed at the hardware-signal level (no logic analyzer access), but
the evidence - glitch timing precisely bracketing the flash-writing
calls, glitch gone once those calls are removed - is strong enough to
act on.

**Ruled out:**
- **Double-buffered DPI panel + `esp_lvgl_port`'s tear-avoidance mode**
  (`CONFIG_BSP_LCD_DPI_BUFFER_NUMS=2`,
  `CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y`) - doesn't resolve it, whether
  against three clustered writes or one coalesced write. This protects
  against a CPU-write-vs-DMA-read race within the framebuffer itself, a
  different failure mode from a flash operation stalling bus/cache
  access system-wide - consistent with it not helping here.
- **`CONFIG_SPI_FLASH_AUTO_SUSPEND`** - the one mechanism that could
  plausibly fix this at the source, but ESP-IDF's own docs require
  confirming the specific flash chip supports it before enabling; this
  chip's "generic" detection is a concrete signal it isn't a recognized,
  verified combination. Sending an unsupported suspend command mid-write
  risks real data corruption, not just an ineffective no-op - not a risk
  worth taking to fix a display glitch.
- **Deferring the write to later in the boot sequence** - doesn't
  eliminate the mechanism, just relocates which moment the glitch might
  land on. Rejected once module (M3+) background activity is accounted
  for: modules will generate their own log-worthy events throughout
  active use, not just at boot, so there's no generally "safe" moment to
  defer to.
- **Moving log storage to microSD** (a separate SDMMC bus from the SPI1
  flash/PSRAM path, per
  [hardware.md](../architecture/hardware.md#wireless)) - the
  structurally correct fix, since it avoids the shared-bus mechanism
  entirely, and already the intended future role for that tier (see
  [ADR-0012](ADR-0012-storage-tiers.md)). Deliberately deferred:
  [CLAUDE.md](../../CLAUDE.md) requires HomeDeck work fully on stock hardware without a
  card present, so this can only ever be a fallback-guarded enhancement
  layered on top of internal-flash logging, not a standalone fix, and
  the SDMMC bring-up plus presence-detection/fallback logic is real,
  separate scope.

This will only get more consequential, not less: M3+ modules generate
their own log-worthy events during active use (a Harmony command, a
Kodi playback state change), not just at boot, so a growing rate of
persisted log calls throughout the device's runtime is the actual scope
here, not a one-off boot-sequence nuisance.

## Decision

**`Logger::Log()` no longer performs flash I/O on the calling thread.**
It captures the entry (with a real timestamp, taken immediately) and
pushes it onto a `Queue<Item>`; a dedicated background `Task`
(`WorkerLoop()`) owns all actual `Storage` access. This is the first
production use of both `Task` and `Queue<T>` together on firmware -
previously implemented and unit-tested individually, but not exercised
together on FreeRTOS. Boot-sequence entries persist correctly on
hardware, in order, with accurate individual timestamps.

**The worker batches, rather than writing one entry at a time.** After
waking for the first queued item, it drains everything else already
available (`Queue<T>::TryPop()`, non-blocking) before doing a single
`WriteBatch()` covering the whole group. This is what actually
coalesces near-simultaneous `Log()` calls into one flash write instead
of several - moving the write off the caller's task alone doesn't
reduce how many flash operations happen, only where they happen;
coalescing is what reduces the count. On hardware, the three original
boot-sequence entries now share one write (identical timestamps, one
write instead of three).

**`Queue<T>` gains a stop-token-aware blocking `Pop(std::stop_token)`**
(needs `condition_variable_any`, not plain `condition_variable`, for the
stop_token-aware wait overload) so `WorkerLoop()` can wake and exit
cleanly when `Task`'s destructor requests a stop, and a non-blocking
`TryPop()` for the drain-without-waiting pattern above.

**`Logger::ReadAll()` flushes first.** It pushes a flush marker carrying
a `std::promise<void>` completion signal and blocks until `WorkerLoop()`
has processed everything queued ahead of it, so a caller (e.g. the Web
UI's diagnostics page) never sees a view missing something already
`Log()`'d. This is a deliberate, bounded blocking point on whichever
thread calls `ReadAll()` - acceptable there since it's in response to an
explicit request for current state, not on the hot path this ADR exists
to get off of.

**This does not fully eliminate the underlying glitch risk** - it
reduces the number of flash writes clustered into a visually-critical
window (three writes down to one), but a single write during active
viewing can still glitch on this hardware, including with
double-buffering enabled. No safe, available fix for the remaining
single-write case exists (see Context). This is accepted as a known,
documented hardware/BSP limitation for now - see
[hardware.md](../architecture/hardware.md#display-and-touch).

## Consequences

- [ADR-0019](ADR-0019-structured-logging.md)'s "whole-file overwrite,
  not append" storage decision is unchanged - each `WriteBatch()` still
  rewrites the entire current blob, just for a batch of entries instead
  of one, and now off the caller's task.
- `Queue<T>`'s firmware/FreeRTOS backend question its own header
  previously left open (deferred "to when firmware bring-up actually
  needs it") is resolved: the existing generic
  `std::mutex`/`condition_variable_any` implementation works correctly
  on firmware as-is. A FreeRTOS-native backend (`xQueueCreate`) remains
  a possible future optimization, not something blocking correctness.
- Test coverage in `tests/logger_test.cpp` and `tests/queue_test.cpp`
  (new: `Pop(stop_token)`, `TryPop()`, and a coalescing-specific Logger
  test) exercises the new behavior; the two pre-existing rotation tests
  were updated to force batch boundaries via `ReadAll()`'s flush between
  calls, since rotation now triggers per-batch rather than strictly
  per-call.
- microSD-backed log storage remains a deliberately deferred option -
  see [ADR-0012](ADR-0012-storage-tiers.md) and
  [roadmap.md](../roadmap.md) - motivated by this hardware interaction
  specifically, not just "extended retention" in the abstract. Revisit
  once stock-hardware-without-a-card behavior can be preserved as a
  fallback, not before.
