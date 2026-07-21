# ADR-0022: Panel Init Settle Delay, and Vendoring the BSP Locally

## Status

Accepted

## Context

After [ADR-0021](ADR-0021-xip-from-psram.md) fixed the flash-write display
glitch, a second, distinct symptom appeared: the status bar's black
background rendered as grey rather than black on some boots, converging
to black over several seconds - correlated with, but not caused by, the
same `CONFIG_SPIRAM_XIP_FROM_PSRAM` change.

Direct instrumentation of LVGL's public display events
(`LV_EVENT_REFR_START/READY`, `LV_EVENT_FLUSH_START/FINISH`) on the K145
reference unit ruled out every software explanation: the status bar's
background is written to the framebuffer and flushed in a single pass
lasting microseconds, at construction time, and is never touched again.
Clock-tick event dispatch (a separate instrumentation pass) was also
confirmed to run on a regular ~1030ms cadence throughout boot, ruling
out LVGL task starvation as well. With the framebuffer and rendering
pipeline both proven correct almost instantly, the remaining explanation
is the ST7123 panel's own optical response after power-up - not a
software bug.

The panel's init command table
(`espressif__m5stack_tab5/priv_include/disp_init_data_1.h`) shipped with
`delay_ms=0` on every command, including Sleep Out (`0x11`) and Display
On (`0x29`). MIPI DCS requires at least 120ms after Sleep Out before
further commands; there was also no settle time at all between Display
On and `bsp_display_backlight_on()` (called by app code immediately
after the BSP's init sequence returns) - so the backlight was turning on
before the panel had time to optically settle to its commanded colors.

## Decision

**Added `delay_ms=120` after Sleep Out (`0x11`) and `delay_ms=100` after
Display On (`0x29`)** in the ST7123 init table. Confirmed on hardware:
`Display initialized` now logs ~215ms later than before (matching the
added delays), and the status bar renders solid black consistently
across repeated reboots - the grey/fade symptom is gone.

**Vendored `espressif/m5stack_tab5` as a git-tracked local component**
(`firmware/components/m5stack_tab5/`, referenced via `override_path` in
`firmware/main/idf_component.yml`) rather than patching the
registry-fetched copy in place. `firmware/managed_components/` is
gitignored - a patch made there would silently vanish on the next clean
clone or dependency re-resolution, with no record it ever existed.
`override_path` is ESP-IDF's own mechanism for exactly this case;
confirmed via `firmware/dependencies.lock` showing
`type: local, path: .../firmware/components/m5stack_tab5` and a build
that no longer touches `managed_components/` for this component at all.

## Consequences

- This project now carries a local fork of one BSP component. Future
  upstream `espressif/m5stack_tab5` releases won't be picked up
  automatically for it - bumping the version requires manually
  re-applying this delay to a fresh copy (or reconfirming it's no
  longer needed, if a future upstream release fixes it directly).
  Worth filing upstream at some point, but not blocking on that.
- A pre-existing, unrelated bug surfaced once the background rendered
  correctly and quickly: `StatusBar`'s clock label showed LVGL's default
  `"Text"` placeholder until the first `ClockTickEvent` arrived, up to
  one Clock period late. Fixed by setting it to blank at construction,
  matching how the battery label already reads its value immediately
  rather than waiting for the first tick. A real initial value (not
  blank) would require threading `TimeSource` through `StatusBar` and
  both its callers - deferred, tracked in
  [roadmap.md](../roadmap.md)'s Status bar item.
- [hardware.md](../architecture/hardware.md) doesn't get a new entry for
  this - the panel's settling behavior is now fully accounted for by the
  init sequence, not a standing characteristic a reader needs to know
  about to work on this codebase.
