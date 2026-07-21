# ADR-0019: Structured Logging Format and Storage

## Status

Accepted

## Context

[core.md](../architecture/core.md) names "structured, leveled logging
usable by Core and modules" as a Logging responsibility distinct from
crash/reboot diagnostics ([ADR-0013](ADR-0013-crash-and-reboot-diagnostics.md)),
which is already built. [diagnostics.md](../architecture/diagnostics.md)
requires each entry to carry a timestamp, level, and module/component
tag — "this is what makes filtering by module or severity in the Web UI
possible, rather than grepping an undifferentiated stream" — and
explicitly flags rotation policy and file format as undecided.
[ADR-0012](ADR-0012-storage-tiers.md) already assigns "bounded/rotating
logs" to the internal flash filesystem tier, but not which file format
or rotation scheme.

## Decision

**Storage: reuse `CacheStore`, add no new platform code.** The internal
flash filesystem tier is already mounted by `FirmwareCacheStore`
(`src/platform/firmware/cache_store.cpp`) at `/storage`; a second,
independent FAT mount at the same path from a dedicated log-store class
isn't safe to run alongside it. Logging is built entirely on the
existing `Storage`/`CacheStore` read/write API instead of a new
platform interface — two cache keys (`"log_current"`, `"log_rotated"`)
under a `"core"` namespace. This means every `Log()` call rewrites the
whole current blob (`CacheStore::Write()` is whole-file overwrite, not
append) rather than doing a true file append; accepted for now since it
reuses already-tested code and adds no new platform-specific file I/O.
Revisit only if this proves to be a measured bottleneck, not assumed
upfront.

**Format: JSON-lines**, one JSON object per line
(`{"timestamp":...,"level":...,"component":...,"message":...}`).
**Options considered:**
- A delimited plain-text format (e.g. tab-separated) — simplest to
  write, but needs custom escaping for arbitrary message content
  (embedded delimiters), and doesn't reuse anything already in the
  codebase.
- A binary structured format — most compact, but not human-readable if
  downloaded raw, and needs a custom parser for real, non-hypothetical
  benefit.
- JSON-lines (decided) — reuses `nlohmann::json`, already a project
  dependency used by every existing HTTP handler, so no new dependency
  and no custom escaping logic. Each line is independently valid JSON,
  which also means the log-retrieval endpoint can build its response by
  joining lines with commas inside `[...]` rather than parsing and
  re-serializing.

**Rotation: size-based, single backup.** The current log blob is capped
at 64KB (a placeholder against the storage partition's real 8MB+
budget, tuned like other numeric thresholds in this project once real
usage exists to tune against); crossing the cap moves the current blob
to a single rotated slot and starts fresh. **Time-based (daily-file)
rotation was considered and rejected**: it would need date-stamped
filenames, and the RTC's own already-documented gap — uncalibrated
until Wi-Fi/NTP sets it ([ADR-0016](ADR-0016-battery-rtc-library.md))
— would make those dates unreliable on exactly this hardware, not a
hypothetical edge case.

**Timestamps use the existing wall-clock `TimeSource`**
(`src/platform/time_source.h`, the same source `Clock` already uses),
not a monotonic clock — logs exist for correlating with real-world
events, not duration math, so an approximate wall-clock time (subject
to the same RTC gap noted above) is still more useful than a
meaningless monotonic tick count.

**No runtime-configurable minimum log level** for this pass — callers
decide what's worth persisting by what they call `Log()` with, rather
than adding a settings-backed filter with no real consumer yet.

## Consequences

- [core.md](../architecture/core.md) and
  [diagnostics.md](../architecture/diagnostics.md) state the resulting
  design without repeating this reasoning.
- `Logger` (`src/core/logger.h`/`.cpp`) is entirely target-agnostic
  Core code — no firmware-only mechanism, unlike crash/reboot
  diagnostics, so the simulator uses the real implementation rather
  than mock data.
- Log entries and `ESP_LOGI`/`ESP_LOGE` console output remain distinct
  and unrelated - this ADR doesn't migrate existing console logging
  calls to the new persisted mechanism.
