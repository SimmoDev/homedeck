#pragma once

#include "core/event_bus.h"
#include "lvgl.h"

// The proof-of-mechanism event for the Core Concurrency Abstraction +
// EventBus + dedicated-UI-task chain - see docs/roadmap.md's M1 items.
// Not a real product event; deliberately trivial.
struct HeartbeatEvent {
    int count;
};

// Owns its widget tree and its EventBus subscription directly (see
// ADR-0004's screen-controller pattern) - subscribes via SubscribeUi so
// the callback is guaranteed to already be running safely on the UI task
// (see ADR-0011), unsubscribes automatically on destruction.
class HeartbeatScreenController {
public:
    HeartbeatScreenController(lv_obj_t* parent, homedeck::EventBus& event_bus);

private:
    lv_obj_t* label_;
    homedeck::EventBus::ScopedSubscription subscription_;
};
