# Touch UI

The Touch UI is the on-device LVGL interface used for everyday operation:
Harmony control, Kodi browsing, media playback, Home Assistant control,
monitoring dashboards, and the dashboard itself. See
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md) for the philosophy behind
the decisions recorded here, and [dashboard.md](dashboard.md) for the home
screen specifically.

## Priorities

Per CLAUDE.md, the Touch UI prioritizes:

- Speed
- Simplicity
- Large touch targets
- Minimal interaction steps

Complex administration features are explicitly out of scope for this
surface — those belong in the [Web Management UI](web-ui.md).

## Navigation model

Screens are registered as routes with a central navigation manager owned by
Core (see [core.md](core.md#responsibilities)), rather than screens directly
pushing/popping each other. This guarantees two invariants hold across every
screen, including ones added by future modules:

- The user always has a predictable home screen (the dashboard).
- The user always has a simple, consistent way to return home.

Modules register screens with this navigation manager; they do not manage
their own screen stack.

The navigation manager is the backend routing mechanism behind the second
invariant; the actual on-screen affordance the user taps to invoke it is a
persistent home icon at a fixed screen location, present on every screen
except the dashboard itself — not a gesture or a hardware button. See
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md#decision-return-home-affordance)
for why a persistent affordance was chosen over an edge-swipe gesture or a
power-button long-press.

## Rendering

LVGL is the rendering toolkit, driven through M5GFX/M5Unified on-device and
through the SDL2 desktop backend in the [simulator](simulator.md). UI code
should target LVGL's widget APIs and the hardware-facing interfaces
described in [overview.md](overview.md#hardware-abstraction), not
M5Unified/M5GFX or ESP-IDF APIs directly — this is what keeps UI code
portable to the simulator.

A single dedicated UI task owns LVGL exclusively — it runs the
`lv_timer_handler()` loop and is the only task that ever calls an LVGL
function directly. This holds identically on firmware and the simulator,
since it's a constraint of LVGL itself, not the display driver. See
[Thread safety](#thread-safety) below for why this matters and what it
means for screen controllers.

## State updates

UI components subscribe to Core's event bus for the state they display and
update in response to events, rather than polling. See
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md#event-driven-ui-updates-no-polling-from-the-ui-layer)
for the rationale, and the [per-screen controller
pattern](../decisions/ADR-0004-ui-philosophy.md#decision-ui-state-management-pattern)
decided there for how individual screens should be structured.

## Thread safety

LVGL is not thread-safe — every `lv_*` call must happen on the UI task
described above. Module background tasks (see
[modules.md](modules.md#what-a-module-may-provide)) run on their own Core
`Task` (the [Core Concurrency
Abstraction](../decisions/ADR-0002-technology-stack.md#decision-core-concurrency-abstraction) —
FreeRTOS-backed on firmware, C++ standard library-backed on the simulator,
never touched directly) and publish events from there; a screen
controller's event-subscription callback must not touch LVGL objects
directly from whatever task the event was published on. Core's event bus
guarantees this hand-off for UI subscriptions specifically — by the time a
screen controller's callback runs, it is already executing safely on the
UI task.
Screen controllers do not need their own LVGL threading logic; the
guarantee is structural, not a discipline each screen author has to
remember. See [ADR-0011](../decisions/ADR-0011-lvgl-thread-safety.md) for
why this was chosen over a global LVGL mutex or leaving it to each screen
controller, and for how event payload data (not just the callback
trigger) stays valid across the deferred hand-off, since the callback
doesn't run until the next LVGL tick, not immediately.

## Notification presentation

Core's notification service (see [core.md](core.md#responsibilities))
deliberately decouples modules from how a notification is shown — this
section is that "how," for the Touch UI specifically. CLAUDE.md names four
possible outputs; each maps onto something HomeDeck already has, or
explicitly doesn't yet:

- **Screen banners** — a transient overlay on whatever screen is currently
  active (including the dashboard), not a navigation change. Deferred
  notifications (see
  [ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md#decision-alert-priority-wake-cycle-during-sleeping))
  surface this way on next wake.
- **Sound** — uses the confirmed ES8388 codec and onboard speaker (see
  [hardware.md](hardware.md#audio)) via the audio-out hardware interface
  already named in [overview.md](overview.md#hardware-abstraction). Exact
  sound selection per notification severity is an M2 implementation detail,
  not an architectural decision.
- **Dashboard indicators** — a notification-count/status badge surfaced
  through the existing widget system (see
  [dashboard.md](dashboard.md#widget-system)), not a separate mechanism.
- **Vibration** — CLAUDE.md names this as explicitly "future," and
  correctly so: no haptic motor exists on the confirmed Tab5 BOM (see
  [hardware.md](hardware.md)). Not planned until hardware exists to support
  it, and not a gap in the current design.

Which output(s) a given notification uses is driven by its urgency (the
alert-priority/deferred concept from
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md)), not a
per-module choice — consistent with Core owning presentation policy while
modules only supply the notification's content.

## Status

The dashboard exists (`DashboardScreen` in `src/ui/screens/`) — the first
real Touch UI, replacing the throwaway proof-of-mechanism screen from the
previous M1 item. It's the only screen so far; the persistent home
affordance described above has nothing to attach to until a second,
non-dashboard screen exists (see [roadmap.md](../roadmap.md)), so neither
it nor a Navigation manager are built yet.
