#include "core/event_bus.h"

#include <algorithm>

namespace homedeck {

EventBus::ScopedSubscription::ScopedSubscription(EventBus* bus, std::type_index type,
                                                   std::uint64_t id)
    : bus_(bus), type_(type), id_(id) {}

EventBus::ScopedSubscription::ScopedSubscription(ScopedSubscription&& other) noexcept
    : bus_(other.bus_), type_(other.type_), id_(other.id_) {
    other.bus_ = nullptr;
}

EventBus::ScopedSubscription& EventBus::ScopedSubscription::operator=(
    ScopedSubscription&& other) noexcept {
    if (this != &other) {
        Reset();
        bus_ = other.bus_;
        type_ = other.type_;
        id_ = other.id_;
        other.bus_ = nullptr;
    }
    return *this;
}

void EventBus::ScopedSubscription::Reset() {
    if (bus_ != nullptr) {
        bus_->Unsubscribe(type_, id_);
        bus_ = nullptr;
    }
}

void EventBus::SetUiDispatcher(UiDispatcher dispatcher) {
    std::lock_guard<std::mutex> lock(mutex_);
    ui_dispatcher_ = std::move(dispatcher);
}

EventBus::ScopedSubscription EventBus::SubscribeImpl(
    std::type_index type, bool is_ui, std::function<void(std::shared_ptr<void>)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t id = next_id_++;
    subscribers_[type].push_back(Subscriber{id, is_ui, std::move(callback)});
    return ScopedSubscription(this, type, id);
}

void EventBus::Unsubscribe(std::type_index type, std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(type);
    if (it == subscribers_.end()) return;
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                               [id](const Subscriber& s) { return s.id == id; }),
               list.end());
}

void EventBus::PublishImpl(std::type_index type, std::shared_ptr<void> payload) {
    // Copy the relevant subscriber list (and dispatcher) out from under
    // the lock before invoking callbacks, so a subscriber that calls
    // back into the bus (subscribing/publishing from within a callback)
    // doesn't deadlock on this non-reentrant mutex.
    std::vector<Subscriber> subscribers_to_notify;
    UiDispatcher dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = subscribers_.find(type);
        if (it != subscribers_.end()) {
            subscribers_to_notify = it->second;
        }
        dispatcher = ui_dispatcher_;
    }

    for (const auto& subscriber : subscribers_to_notify) {
        if (subscriber.is_ui) {
            if (!dispatcher) continue;  // no UI task registered yet; drop.
            dispatcher([callback = subscriber.callback, payload]() { callback(payload); });
        } else {
            subscriber.callback(payload);
        }
    }
}

}  // namespace homedeck
