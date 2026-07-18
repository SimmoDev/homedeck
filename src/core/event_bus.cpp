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

std::function<void(std::shared_ptr<void>)> EventBus::FindCallback(std::type_index type,
                                                                    std::uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(type);
    if (it == subscribers_.end()) return nullptr;
    auto sub_it = std::find_if(it->second.begin(), it->second.end(),
                                [id](const Subscriber& s) { return s.id == id; });
    if (sub_it == it->second.end()) return nullptr;
    return sub_it->callback;
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
            // Captures id, not the callback itself - lv_async_call() (or
            // whatever the registered dispatcher defers through) may run
            // this well after the subscriber unsubscribed, so liveness is
            // re-checked by looking the callback up again at the point it
            // actually executes, not at the point it was queued. See
            // ADR-0011's "Resolved (M2)" note: capturing the callback
            // directly here would let a deferred call fire against an
            // already-destroyed subscriber.
            dispatcher([this, type, id = subscriber.id, payload]() {
                auto callback = FindCallback(type, id);
                if (callback) callback(payload);
            });
        } else {
            subscriber.callback(payload);
        }
    }
}

}  // namespace homedeck
