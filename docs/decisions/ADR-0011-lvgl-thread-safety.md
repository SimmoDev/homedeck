# ADR-0011: LVGL Thread Safety

## Status

Accepted

## Context

LVGL is not thread-safe: all `lv_*` API calls, including widget creation
and mutation, must happen from a single consistent execution context —
either always the same task, or protected by a mutex held for the full
duration of any LVGL call. LVGL also requires a periodic driver task that
calls `lv_timer_handler()` (and feeds `lv_tick_inc()`) regularly to drive
rendering, animation, and input processing.

HomeDeck's architecture has module background tasks (see
[modules.md](../architecture/modules.md#what-a-module-may-provide)), each
potentially running on its own Core `Task` (the [Core Concurrency
Abstraction](ADR-0002-technology-stack.md#decision-core-concurrency-abstraction) —
FreeRTOS-backed on firmware, C++ standard library-backed on the simulator),
publishing events through Core's event bus (see
[overview.md](../architecture/overview.md#event-driven-design)). Touch UI
screen controllers (see
[ADR-0004](ADR-0004-ui-philosophy.md#decision-ui-state-management-pattern))
subscribe to these events and update LVGL widgets in response. Nothing in
the architecture as described before this ADR specifies how event delivery
crosses this thread boundary safely — if a subscriber's callback runs
synchronously on the publishing module's task and touches LVGL objects
directly, that's a real, common RTOS+LVGL bug, not a theoretical one. This
applies equally to the simulator (SDL2 desktop driver) and firmware
(M5GFX at the time this ADR was written; display/touch specifically now
use `espressif/m5stack_tab5` instead, per
[ADR-0014](ADR-0014-hardware-support-library.md) — doesn't change this
ADR's reasoning, since LVGL's core thread-safety requirement is
independent of the display driver either way).

## Decision

**Options:**
- A dedicated UI task owns LVGL exclusively — runs the `lv_timer_handler()`
  loop and is the only task that ever calls `lv_*` functions. Any other
  task affecting the UI hands off to it via LVGL's own `lv_async_call()`
  primitive, LVGL's officially recommended mechanism for exactly this case.
- A global LVGL mutex, taken by any task before calling any `lv_*`
  function and released after.
- No framework-level guarantee — each screen controller is individually
  responsible for remembering to marshal its own LVGL calls safely.

**Decided:** a dedicated UI task owns LVGL exclusively, using
`lv_async_call()` as the hand-off mechanism. Core's event bus dispatch to
UI subscribers is responsible for this hand-off — a screen controller's
event-subscription callback is guaranteed to already be executing safely
on the UI task by the time it runs, regardless of which task published the
event. Screen controllers therefore don't need their own LVGL threading
knowledge; the guarantee lives at the event bus's UI-dispatch boundary,
not at each call site.

A global mutex was rejected as the weaker option: it's easy to forget at
any individual call site (a silent, intermittent bug rather than a
structural guarantee), and holding a shared UI mutex from a low-priority
background task risks priority inversion against high-priority input
handling. Relying on each screen controller to individually remember to
use `lv_async_call()` was rejected for the same reason the sleep-veto and
alert-priority mechanisms weren't left to per-module discretion (see
[ADR-0005](ADR-0005-power-and-sleep-model.md)) — a correctness requirement
that depends on every future author remembering it correctly, rather than
being structurally guaranteed, is exactly the kind of thing that should be
centralized.

This does not change the event bus's public shape for non-UI subscribers
(e.g. a future Web UI WebSocket relay subscribing to the same events) —
they have no *LVGL* constraint, so they don't need the `lv_async_call()`
hand-off specifically. That does not make them exempt from dispatch-safety
concerns generally: the WebSocket relay has its own, different
dispatch-safety requirement (`esp_http_server`'s connection state is no
safer to touch from an arbitrary task than LVGL's is) — see
[ADR-0002](ADR-0002-technology-stack.md#3-embedded-webwebsocket-server)
for what that requirement actually is and what's still unconfirmed about
it. The general lesson generalizes beyond LVGL: *any* dedicated-resource
subscriber needs its own hand-off, evaluated on its own terms, not assumed
safe by default just because it isn't LVGL.

## Decision: Event payload lifetime across the dispatch boundary

**Context:** `lv_async_call()` doesn't execute its callback immediately —
it queues the call for the *next* `lv_timer_handler()` tick, and takes a
single `void*` user-data argument. If an event carries a payload (e.g.
"ActivityChanged" carrying the new activity name/ID) and the event bus
passes a pointer into the publishing module's transient state — a stack
local, or any buffer the publisher might reuse or free once its call
returns — that pointer can be dangling by the time the deferred callback
actually runs. This is a real use-after-free risk, not a hypothetical one,
and it would surface as an intermittent, timing-dependent crash rather
than a reliably reproducible one — exactly the kind of bug that's cheap to
prevent architecturally and expensive to debug once shipped.

**Options:**
- Heap-allocate a copy of the payload at publish time, freed by the UI-side
  callback after it's consumed — simple, but manual free-after-consume
  bookkeeping is itself a bug risk if ever done inconsistently.
- Reference-counted shared ownership (e.g. `std::shared_ptr`) — the
  payload's lifetime is automatically extended until the last reference
  (including the one held by the pending async call) is released; no
  manual free bookkeeping.
- A fixed-size pool/ring buffer of preallocated payload slots — avoids
  heap churn entirely, but caps concurrent in-flight events and needs a
  defined policy for what happens if the pool is exhausted under a burst.

**Decided:** reference-counted shared ownership. Event payloads are copied
into a heap-allocated, reference-counted object at publish time (not
referenced via a raw pointer into the publisher's transient state), and
the `void*` handed to `lv_async_call()` is that shared ownership, released
when the UI-side callback finishes with it. This fits the project's
existing "modern C++, RAII" coding standard directly, and removes the
manual-bookkeeping risk of plain heap-allocate-and-free. The pool/ring
buffer option was not chosen as the default: given HomeDeck's events are
user-facing state changes (activity changed, monitor down), not a
high-frequency sensor stream, the heap churn a pool would exist to avoid
isn't a real problem here — this can be revisited if a real event-rate
problem shows up in practice, but shouldn't be built pre-emptively against
a load that doesn't exist.

## Consequences

- [ui.md](../architecture/ui.md) states this as the current design; this
  ADR is the reference for why.
- Core's event bus implementation must distinguish UI-facing subscriptions
  (which get the `lv_async_call()` hand-off) from other subscribers (which
  don't need it) — a concrete interface detail for M1/M2 implementation,
  not fixed further by this ADR.
- Event payload types must be designed for reference-counted copy
  (ordinary value types wrapped in shared ownership by the bus, not types
  that assume a single owner or a stack-bound lifetime) from the first
  event type onward — retrofitting this after several event types already
  assume raw-pointer semantics would touch every publisher.
- The UI task's `lv_timer_handler()` loop is itself a Core/platform
  responsibility, not something any screen or module manages — consistent
  with [core.md](../architecture/core.md)'s Navigation responsibility
  already owning screen orchestration.
- This applies identically on firmware and the simulator, since it's an
  LVGL core constraint independent of the display driver — no divergence
  risk of the kind flagged for the HTTP/WebSocket transport in
  [ADR-0002](ADR-0002-technology-stack.md#3-embedded-webwebsocket-server).
- **Known gap, not yet solved:** `lv_async_call()` hands off *when* a
  callback runs safely, not *whether* the subscriber it belongs to still
  exists by then. `EventBus::Unsubscribe` (triggered by a
  `ScopedSubscription` going out of scope — see
  [ADR-0004](ADR-0004-ui-philosophy.md#decision-ui-state-management-pattern))
  only stops *future* publishes from including that subscriber; if a
  publish already ran on a non-UI thread and queued the hand-off before
  `Unsubscribe` completes, that queued callback still fires afterward,
  against a subscriber that may by then be destroyed. Harmless today
  because nothing yet destroys a screen at runtime (`Navigation` caches
  every registered screen for the process's lifetime), but this becomes a
  real, timing-dependent use-after-free the moment something is destroyed
  while subscribed to an event published from another thread — which
  `Clock`'s `Timer` already does. Must be resolved before that becomes
  possible; see the widget framework item in
  [roadmap.md](../roadmap.md#m2--platform-services-current).
