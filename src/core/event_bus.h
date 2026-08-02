#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace homedeck {

// The publish/subscribe backbone connecting UI, Core services, and
// modules - see docs/architecture/overview.md#event-driven-design.
//
// Core must never depend on LVGL or any specific UI implementation (the
// same layer rule that keeps Core from depending on a module - see
// overview.md), so EventBus never calls lv_async_call() itself: UI-safe
// delivery goes through a dispatch function registered via
// SetUiDispatcher() - both targets register ui/ui_dispatch.h's
// PostToUiThread. This also means EventBus is fully unit-testable
// without linking LVGL at all.
class EventBus {
public:
    using UiDispatcher = std::function<void(std::function<void()>)>;

    // RAII handle: unsubscribes automatically when destroyed or Reset(),
    // so a screen controller's "unsubscribe on exit" (see ADR-0004) is
    // just "let this member go out of scope," not a call site to
    // remember.
    //
    // Safe even against a SubscribeUi callback already dispatched from a
    // publish on another thread (e.g. Clock's Timer) before this handle
    // is destroyed: PublishImpl re-checks whether the subscriber is still
    // registered at the point the deferred call actually executes, not
    // at the point it was queued, so a callback belonging to an
    // already-destroyed subscriber is never invoked (see ADR-0011's
    // "Resolved (M2)" note).
    class ScopedSubscription {
    public:
        ScopedSubscription() = default;
        ~ScopedSubscription() { Reset(); }

        ScopedSubscription(ScopedSubscription&& other) noexcept;
        ScopedSubscription& operator=(ScopedSubscription&& other) noexcept;

        ScopedSubscription(const ScopedSubscription&) = delete;
        ScopedSubscription& operator=(const ScopedSubscription&) = delete;

        void Reset();

    private:
        friend class EventBus;
        ScopedSubscription(EventBus* bus, std::type_index type, std::uint64_t id);

        EventBus* bus_ = nullptr;
        std::type_index type_ = typeid(void);
        std::uint64_t id_ = 0;
    };

    // Called once, by UiTask at startup. Must be called before any
    // SubscribeUi subscriber's event is published, or that publish is
    // silently dropped for UI subscribers (see PublishImpl).
    void SetUiDispatcher(UiDispatcher dispatcher);

    // Delivered synchronously, on the publishing task - for subscribers
    // that either run on the same task as the publisher or have their
    // own dispatch-safety handling (see ADR-0011's WebSocket-relay note).
    template <typename T>
    [[nodiscard]] ScopedSubscription Subscribe(std::function<void(const T&)> callback);

    // Delivered through the registered UI dispatcher - the callback is
    // guaranteed to already be running safely on the UI task by the time
    // it executes (see ADR-0011).
    template <typename T>
    [[nodiscard]] ScopedSubscription SubscribeUi(std::function<void(const T&)> callback);

    // Copies payload into reference-counted shared ownership at publish
    // time (see ADR-0011's event payload lifetime decision) - never a
    // raw pointer into the caller's transient state.
    template <typename T>
    void Publish(T payload);

private:
    struct Subscriber {
        std::uint64_t id;
        bool is_ui;
        std::function<void(std::shared_ptr<void>)> callback;
    };

    ScopedSubscription SubscribeImpl(std::type_index type, bool is_ui,
                                      std::function<void(std::shared_ptr<void>)> callback);
    void Unsubscribe(std::type_index type, std::uint64_t id);
    // Re-looks-up a subscriber's callback by id at the time it's called,
    // rather than assuming a copy taken earlier is still valid - see
    // PublishImpl's UI dispatch path and ADR-0011's Consequences section
    // (the subscriber-liveness bullet). Returns nullptr if the
    // subscriber has since unsubscribed.
    std::function<void(std::shared_ptr<void>)> FindCallback(std::type_index type,
                                                              std::uint64_t id);
    void PublishImpl(std::type_index type, std::shared_ptr<void> payload);

    std::mutex mutex_;
    UiDispatcher ui_dispatcher_;
    std::uint64_t next_id_ = 1;
    std::unordered_map<std::type_index, std::vector<Subscriber>> subscribers_;
};

template <typename T>
EventBus::ScopedSubscription EventBus::Subscribe(std::function<void(const T&)> callback) {
    return SubscribeImpl(std::type_index(typeid(T)), /*is_ui=*/false,
                          [callback = std::move(callback)](std::shared_ptr<void> payload) {
                              callback(*std::static_pointer_cast<T>(payload));
                          });
}

template <typename T>
EventBus::ScopedSubscription EventBus::SubscribeUi(std::function<void(const T&)> callback) {
    return SubscribeImpl(std::type_index(typeid(T)), /*is_ui=*/true,
                          [callback = std::move(callback)](std::shared_ptr<void> payload) {
                              callback(*std::static_pointer_cast<T>(payload));
                          });
}

template <typename T>
void EventBus::Publish(T payload) {
    auto shared_payload = std::make_shared<T>(std::move(payload));
    PublishImpl(std::type_index(typeid(T)), std::static_pointer_cast<void>(shared_payload));
}

}  // namespace homedeck
