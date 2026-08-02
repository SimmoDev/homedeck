# Touch UI

The Touch UI is the on-device LVGL interface used for everyday operation:
Harmony control, Kodi browsing, media playback, Home Assistant control,
monitoring dashboards, and the dashboard itself. See
[ADR-0004](../decisions/ADR-0004-ui-philosophy.md) for the philosophy behind
the decisions recorded here, and [dashboard.md](dashboard.md) for the home
screen specifically.

## Priorities

Per [CLAUDE.md](../../CLAUDE.md), the Touch UI prioritizes:

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

`WifiSetupScreen` (see [status](#status) below) is a deliberate, narrow
exception to "every screen except the dashboard" above: going home before
Wi-Fi is configured would strand the user on a screen with no network and
no way back to this one short of a reboot, since nothing else ever
navigates here automatically. No other screen is expected to need this
exception — a real navigable path away from the dashboard should still
get the home affordance.

A separate, independent piece of persistent chrome exists alongside the
home affordance — a status bar (`StatusBar`, `src/ui/status_bar.h`/`.cpp`)
showing date/time and battery on every screen, dashboard included; it's
not part of the navigation manager and doesn't route anywhere.
**Implemented**, constructed by each screen the same way each screen
constructs its own home affordance — see [dashboard.md](dashboard.md#status)
for implementation status and [ADR-0008](../decisions/ADR-0008-dashboard-widget-system.md#decision-status-bar-vs-dashboard-only-widgets)
for why it exists and why it's kept separate from the home affordance for
now.

## Object lifecycle

Every screen and module-registered screens alike are constructed once for
the program's lifetime today, so no LVGL object built on this codebase
has ever actually been torn down in practice. That won't hold once a
module screen is created and later navigated away from (e.g. a Harmony
activity detail screen), so the ownership rule below is a requirement
now, not a followup:

- An LVGL object created as a child of another object the same class
  already owns (directly or transitively) needs no cleanup of its own —
  `lv_obj_del()` on the owning root recurses through every child
  automatically. This covers most widgets and screen chrome (e.g.
  `DashboardGrid`'s widgets, `StatusBar`, `OnScreenKeyboard`), since they
  take a `parent` and construct themselves as its child.
- A class that creates an LVGL object with no such longer-lived owner —
  a screen's own root via `lv_obj_create(nullptr)`, or a `lv_layer_top()`
  overlay like `NotificationBanner`/`QuickSettingsPanel` — must delete
  that object in its own destructor. Nothing else will.
- An `lv_timer_t*` is a separate resource from LVGL's object tree —
  deleting a parent object does not delete a timer, even one whose user
  data points at that object. A class holding one (e.g.
  `NotificationBanner`'s `dismiss_timer_`) must delete it in its own
  destructor too, and before deleting the object the timer's callback
  reads, not after.

## Rendering

LVGL is the rendering toolkit, driven through the hardware BSP on-device
(see [overview.md](overview.md#hardware-abstraction) for which library)
and through the SDL2 desktop backend in the [simulator](simulator.md). UI
code should target LVGL's widget APIs and the hardware-facing interfaces
described in [overview.md](overview.md#hardware-abstraction), not the
BSP or ESP-IDF APIs directly — this is what keeps UI code portable to
the simulator.

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
section is that "how," for the Touch UI specifically. [CLAUDE.md](../../CLAUDE.md) names four
possible outputs; each maps onto something HomeDeck already has, or
explicitly doesn't yet:

- **Screen banners** — a transient overlay on whatever screen is currently
  active (including the dashboard), not a navigation change. A
  notification published while the display is off (Sleeping) becomes
  visible this way once it's back on, whether that's an immediate
  render that was simply invisible with the backlight off, or a
  deliberate replay — an implementation detail not yet decided.
- **Sound** — uses the confirmed ES8388 codec and onboard speaker (see
  [hardware.md](hardware.md#audio)) via `AudioOutput`
  (`src/platform/audio_output.h`), the portable audio-out hardware
  interface already named in
  [overview.md](overview.md#hardware-abstraction) — real, via
  `NotificationSound` (`src/core/notification_sound.h`/`.cpp`), which
  plays the same short tone for every `NotificationEvent` regardless of
  severity today, even though `kAlertPriority` already has a real
  publisher (`CriticalBatteryMonitor` — see
  [power-management.md](power-management.md#status)). Differentiating
  the sound per severity is a deliberately deferred M7 sound-design
  follow-up, not something blocked on a second severity existing.
- **Dashboard indicators** — real, via `NotificationWidget`
  (`src/ui/notification_widget.h`/`.cpp`), surfaced through the existing
  widget system (see [dashboard.md](dashboard.md#widget-system)), not a
  separate mechanism. A last-notification tile (always echoes the most
  recent message), not an unread-count badge - that variant is a
  separate, deliberately deferred M7 follow-up (see
  [roadmap.md](../roadmap.md)), since nothing today fires more than one
  notification per episode.
- **Vibration** — [CLAUDE.md](../../CLAUDE.md) names this as explicitly "future," and
  correctly so: no haptic motor exists on the confirmed Tab5 BOM (see
  [hardware.md](hardware.md)). Not planned until hardware exists to support
  it, and not a gap in the current design.

Which output(s) a given notification uses is driven by its urgency (the
alert-priority/deferred concept from
[ADR-0005](../decisions/ADR-0005-power-and-sleep-model.md)), not a
per-module choice — consistent with Core owning presentation policy while
modules only supply the notification's content.

## Status

The dashboard is implemented (`DashboardScreen` in `src/ui/screens/`),
and so are Navigation (`src/ui/navigation.h`) and the persistent home
affordance (`src/ui/home_affordance.h`) — a minimal route registry
(`Register`/`GoTo`/`GoHome`) and a reusable `LV_SYMBOL_HOME` button,
built on both firmware and the simulator. `WifiSetupScreen`
(`src/ui/screens/wifi_setup_screen.h`/`.cpp`), the Touch UI fallback for
initial Wi-Fi setup (see
[networking.md](networking.md#initial-wi-fi-provisioning)), is the
genuine second screen registered alongside the dashboard on both
targets (it omits the home affordance itself - see the Navigation model
section above). Below its Connect button it shows instructions for
setting up from a computer/phone instead (join the SoftAP SSID, then
browse to the gateway IP), and a connect-failure message once
`wifi_setup.cpp` gives up retrying a freshly-submitted set of credentials
(`SetConnectError`, cleared again as soon as the user retries) - all
screen text, including
`OnScreenKeyboard`'s key labels (`src/ui/keyboard_input.h`/`.cpp` - a
reusable on-screen keyboard that attaches to any `lv_textarea`, with no
knowledge of what the text is for, so future screens needing text entry
can reuse it directly rather than each wiring `lv_keyboard` themselves),
uses Montserrat 24 (see [dashboard.md](dashboard.md#status)) rather than
LVGL's small default - the keyboard needs this set directly on its own
`LV_PART_ITEMS` style, since `lv_buttonmatrix` (what the keyboard is
built on) doesn't inherit an ancestor's font for button labels the way
plain labels do.

Which screen loads first is decided before the dashboard is ever
constructed: firmware shows a brief splash (`ShowSplashScreen()` in
`firmware/main/homedeck.cpp`) immediately after display start, then
brings up just enough of the Wi-Fi subsystem to answer "are credentials
already stored" (`InitWifiAndCheckStoredCredentials()`,
`firmware/main/wifi_setup.h`/`.cpp`) before `Navigation` and the actual
screens are constructed - so an unprovisioned device never shows the
dashboard even briefly before redirecting to setup. The slower
association/DHCP step still happens in the background after that,
unchanged.

Cached-vs-live data distinction on the dashboard when accessed without a
Wi-Fi connection - relevant once a network-dependent widget (e.g.
weather) exists - is covered by the existing offline-behaviour contract
(see [networking.md](networking.md#offline-behaviour)), not something
new this screen needed to solve. The status bar described above is
implemented (see [dashboard.md](dashboard.md#status)).

Notification presentation is implemented: `NotificationBanner`
(`src/ui/notification_banner.h`/`.cpp`) is the screen-banner output —
parented to LVGL's own top layer (`lv_layer_top()`), not any particular
screen, since it renders above whatever screen Navigation currently has
loaded; a single instance covers every screen, unlike `StatusBar`,
which each screen constructs its own copy of. Auto-dismisses a few
seconds after showing. `NotificationSound`
(`src/core/notification_sound.h`/`.cpp`) is the sound output, and
`NotificationWidget` (`src/ui/notification_widget.h`/`.cpp`) is the
dashboard-indicator output — see [Notification
presentation](#notification-presentation) above for both. All three
outputs replace rather than queue: a notification arriving before the
previous one has finished showing/playing simply overwrites it (one
banner, one tone, one dashboard tile), the same "nothing today fires
more than one notification per episode" reasoning the last-notification
tile above already documents, applied consistently across all three.
