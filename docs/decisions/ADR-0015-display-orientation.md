# ADR-0015: Display Orientation

## Status

Accepted

## Context

The Tab5's spec sheet describes its panel as "5\" 1280×720 IPS" — landscape
figures, and what the simulator's window dimensions originally assumed
(`simulator/main.cpp`, before this decision). Real hardware bring-up (see
[ADR-0014](ADR-0014-hardware-support-library.md) and
[hardware.md](../architecture/hardware.md#display-driver-strategy))
found the panel actually reports `720x1280` at runtime — portrait, not
landscape — and that this is genuinely the panel's native scan direction,
not a default init flag: `espressif/m5stack_tab5` hardcodes it as
`BSP_LCD_H_RES`/`BSP_LCD_V_RES` in its own `display.h`, with no
`swap_xy` applied during panel init.

This left a real, project-wide choice open: build the UI for the panel's
native portrait orientation, or rotate to the spec sheet's landscape
figures. Every future screen — not just the initial dashboard — inherits
whichever choice this makes, so it needed resolving deliberately rather
than by whichever way the simulator happened to default.

## Decision

**Options considered:**

- **Rotate to landscape** (`bsp_display_rotate()`, wrapping LVGL's
  `lv_disp_set_rotation()`) — matches the spec sheet's marketed
  orientation. Rejected: the BSP's own source flags that its anti-tearing
  mode isn't supported under software rotation, and a different project
  (ESPHome) hit an open, unresolved bug combining LVGL + 90° rotation on
  this same MIPI-DSI panel class — a blank screen from a driver/LVGL
  dimension mismatch
  ([esphome/esphome#10740](https://github.com/esphome/esphome/issues/10740)).
  Real, corroborated risk on this exact hardware class, not a
  hypothetical one, on top of the CPU cost of rotating every frame.
- **Portrait, native, no rotation** (decided) — the panel's own scan
  direction, with no rotation cost and no exposure to the bug above.
  Also supported by physical evidence: the battery pack protrudes ~1.4cm
  off the back and tilts the device up at that edge when laid flat (see
  [Physical form factor](../architecture/hardware.md#physical-form-factor)).
  Held portrait with the battery at the top, that tilt becomes a passive
  kickstand — an easel angle facing the viewer. Held landscape, the same
  tilt is a sideways lean off the right edge, which doesn't usefully
  raise the screen toward the viewer.

**Decided:** portrait, `720x1280`, no rotation. `DashboardScreen`'s
widgets needed no layout changes to adopt it — both are placed with
`LV_ALIGN_CENTER`/`LV_ALIGN_TOP_RIGHT` relative to the parent, not fixed
coordinates, and the same holds for the home affordance
(`LV_ALIGN_BOTTOM_LEFT`) and the dev-only nav button
(`LV_ALIGN_BOTTOM_MID`) — confirmed by building and running the
simulator at the new resolution with no widget-code edits. Firmware
needed no change at all: `homedeck.cpp` never called
`bsp_display_rotate()`, so it was already running the resolved
orientation without anyone deciding to.

The simulator's window (`simulator/main.cpp`) changed from `1280x720` to
`720x1280` to match. Displaying a 1280px-tall logical canvas on a typical
desktop needed a separate fix: `UiTask` gained a `zoom` parameter
(`lv_sdl_window_set_zoom()`), scaling only the on-screen SDL window —
LVGL still lays out at the full `720x1280` logical resolution
underneath, so simulator layout behavior matches hardware exactly. No
single zoom value fits every developer's desktop/taskbar layout, so it's
runtime-overridable via the `HOMEDECK_SIM_ZOOM` environment variable
(`main.cpp`'s compiled-in default is `0.75`, chosen for clean integer
window dimensions) rather than fixed in code.
**Known, accepted trade-off, independent of the exact value chosen:**
LVGL's SDL backend softens text at any zoom other than `1.0`. `UiTask`
sets `SDL_HINT_RENDER_SCALE_QUALITY` to `"best"` (anisotropic filtering
where the renderer supports it), which measurably reduces this on a real
GPU-accelerated desktop, but not on a software renderer (Xvfb, CI),
which silently ignores anisotropic filtering — worth knowing before
trusting a headless comparison of scale-quality settings. It reduces the
softening, not eliminates it — only `zoom=1.0` does that. Real hardware
is unaffected either way — it renders at native resolution with no
scaling step.

## Consequences

- Every future screen (M2 settings, M3+ module UIs) is portrait-first
  from here on — this is the orientation baseline for the whole product,
  not just the dashboard.
- The dashboard's physical top edge (battery, camera) is now a
  meaningful UI constraint worth remembering for widget placement near
  the top of the screen — it's also where the device physically tilts
  toward the viewer when propped up.
- Simulator text will never render pixel-perfect below `zoom=1.0`. For
  pixel-perfect inspection, run with `HOMEDECK_SIM_ZOOM=1.0` (the window
  will then exceed a typical 1080p desktop's usable height) — see
  [simulator/README.md](../../simulator/README.md).
- Not resolved here: whether a future settings/accessibility option
  should let a user override orientation at runtime. Out of scope until
  there's a real reason to support it — this ADR fixes the *default* and
  *only* supported orientation for now.
