# ADR-0004: UI Philosophy

## Status

Accepted

## Context

[CLAUDE.md](../../CLAUDE.md) defines two distinct interfaces (Touch UI and Web Management UI)
with different priorities, and a dashboard-centric home screen fed by
modules through a standard widget interface. It establishes the goal of
making HomeDeck "feel like a living-room command centre rather than just a
remote launcher." This ADR records the interaction and structural
philosophy that follows from those requirements, so individual screens and
widgets are designed consistently.

## Decision

### Two interfaces, two different design goals

- **Touch UI** optimizes for speed and glanceability during everyday use:
  large touch targets, minimal steps to the common action (start an
  activity, adjust volume, check a monitor), no administrative complexity.
  It is used standing in a living room, often one-handed, often without full
  attention on the screen.
- **Web Management UI** optimizes for completeness and clarity during setup
  and troubleshooting: forms, tables, diagnostics, logs — the things that
  are actively bad UX on a small touch screen and are rarely needed
  day-to-day.

Any feature request should be evaluated against which interface it belongs
in before being designed. A configuration-heavy feature does not belong on
the Touch UI just because it's convenient to add there; a fast, frequent
action does not belong buried in the Web UI.

### Dashboard as home, not a launcher grid

The home screen is a dashboard of live information (time, weather, battery,
current Harmony activity, monitor health, HA states), not a static grid of
app icons. This is a deliberate product stance: HomeDeck should be useful to
glance at even when the user isn't about to issue a command. Apps are
reached from the dashboard/navigation, but the dashboard itself is the
default, persistent view — see
[architecture/dashboard.md](../architecture/dashboard.md).

### Event-driven UI updates, no polling from the UI layer

UI components subscribe to Core's event bus and re-render in response to
events (e.g. `ActivityChanged`, `MonitorStatusChanged`) rather than polling
module or Core state on a timer. Modules own the responsibility of deciding
when their state has actually changed and publishing an event; UI code
should never need to guess an appropriate poll interval. This keeps UI code
simple and keeps polling policy (which affects battery life) centralized in
modules/Core rather than scattered across screens.

### Centralized navigation

Screens are registered as routes with a central navigation manager rather
than screens pushing/popping each other directly. This guarantees the "user
always has a predictable home screen and a simple way to return home"
requirement holds for every screen, including ones added by future modules,
without each module needing to reimplement back/home handling.

**This is a backend routing mechanism, not by itself a UI affordance** — it
guarantees that *if* something asks the navigation manager to go home, it
reliably can, from any screen. It does not by itself give the user
something to tap. See [Decision: Return-home
affordance](#decision-return-home-affordance) below for what actually
delivers this to the user.

## Decision: Return-home affordance

**Context:** the Tab5 has no dedicated navigation button — the only
physical button confirmed on the hardware is the power button, used as a
deep-sleep wake source (see [hardware.md](../architecture/hardware.md#power)),
with its behavior as a general input during Active state unconfirmed. "A
simple, consistent way to return home" must therefore be a software
affordance, and centralized navigation alone doesn't specify what it is.

**Options:**
- A persistent on-screen home affordance (e.g. a small icon at a fixed
  screen location), present on every screen except the dashboard itself.
- An edge-swipe gesture (e.g. swipe from the left edge, or down from the
  top) — no persistent screen space cost.
- A long-press on the physical power button.

**Decided:** a persistent on-screen affordance, as the guaranteed baseline.
This was chosen specifically because it's the only option that can
actually be guaranteed to hold on *every* screen without relying on
unconfirmed assumptions: an edge-swipe gesture risks silent conflicts with
whatever gesture an arbitrary future module's screen assumes is available
to it (e.g. a horizontal swipe through a media carousel, a vertical scroll
gesture near an edge) — the framework can't rule this out for screens that
don't exist yet. A power-button long-press depends on hardware behavior
(whether the button is readable as a general input outside its wake-source
role) that hasn't been verified. Given [CLAUDE.md](../../CLAUDE.md) requires this to hold
universally, not just usually, the option with the fewest unverified
assumptions wins. This doesn't preclude also supporting an edge-swipe
shortcut later as a power-user convenience (M7 — Polish) once the baseline
is proven reliable, but the guaranteed mechanism is the persistent
affordance, not a gesture.

## Decision: UI state-management pattern

[CLAUDE.md](../../CLAUDE.md) does not specify how individual screens should manage local UI
state beyond "event-driven" and "widgets provided through a standard
interface."

**Options:**
- A lightweight per-screen controller pattern: each screen is a small class
  that owns its LVGL widget tree, subscribes to the events it cares about on
  entry, and unsubscribes on exit. No global UI state store.
- A more formal state-management layer (e.g. a central UI state store with
  reducers/observers, similar to Redux/MVU patterns from web frontend
  development).

**Decided: the lightweight per-screen controller pattern.** LVGL is already
a retained-mode widget tree, which itself holds most "state" that matters
for rendering — layering a second state-management framework on top would
duplicate that and add indirection without a corresponding benefit at
HomeDeck's UI complexity. Revisit only if screen state genuinely becomes
hard to reason about in practice (e.g. deeply nested shared state across
many widgets), not pre-emptively.

## Consequences

- Every new screen or widget should be checked against "does this belong on
  Touch UI or Web UI" before implementation, not after.
- Modules are responsible for event hygiene (publish on real state changes,
  not on a timer) since the UI's simplicity depends on it.
- The dashboard widget interface (see
  [architecture/dashboard.md](../architecture/dashboard.md)) is a first-class
  contract, not an afterthought bolted onto module screens.
- The persistent home affordance is part of every screen's baseline layout,
  not an opt-in a module screen could omit — a module registering a screen
  with the navigation manager doesn't get a choice about this, consistent
  with the guarantee needing to hold universally.
