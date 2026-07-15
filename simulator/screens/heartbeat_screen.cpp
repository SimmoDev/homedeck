#include "heartbeat_screen.h"

#include <cstdio>

HeartbeatScreenController::HeartbeatScreenController(lv_obj_t* parent,
                                                       homedeck::EventBus& event_bus) {
    label_ = lv_label_create(parent);
    lv_label_set_text(label_, "HomeDeck Simulator");
    lv_obj_center(label_);

    subscription_ = event_bus.SubscribeUi<HeartbeatEvent>([this](const HeartbeatEvent& event) {
        char text[32];
        std::snprintf(text, sizeof(text), "HomeDeck Simulator\n%d", event.count);
        lv_label_set_text(label_, text);
    });
}
