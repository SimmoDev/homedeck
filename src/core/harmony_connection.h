#pragma once

#include "core/event_bus.h"
#include "core/module.h"
#include "core/retry_backoff.h"
#include "core/storage.h"
#include "platform/http_client.h"
#include "platform/task.h"
#include "platform/websocket_client.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace homedeck {

// See ADR-0029 for the protocol this is built against: a local
// WebSocket/JSON API on the hub's port 8088, no authentication step of
// any kind - "connection," not "authentication," despite the roadmap's
// older wording (docs/roadmap.md's M3 section, corrected there once this
// lands).
enum class HarmonyConnectionState {
    kDisconnected,  // no hub address configured, or not yet attempted
    kConnecting,
    kConnected,
    kError,  // most recent attempt failed; a retry is scheduled
};

struct HarmonyDevice {
    std::string id;
    std::string label;
};

struct HarmonyActivity {
    std::string id;
    std::string label;
};

struct HarmonyConnectionSnapshot {
    HarmonyConnectionState state = HarmonyConnectionState::kDisconnected;
    // True once a config fetch has ever succeeded this session - devices/
    // activities below stay at their last-known values through a
    // subsequent disconnect/error rather than clearing, the same
    // "keep showing the last known state" reasoning WeatherState's own
    // `live` flag follows, except nothing here is persisted to Storage's
    // cache tier - a fresh device/activity list arrives from the hub
    // itself on every reconnect, so there is no offline value in caching
    // it the way weather's data (fetched from a third party, not the
    // device being controlled) genuinely needs.
    bool has_config = false;
    std::vector<HarmonyDevice> devices;
    std::vector<HarmonyActivity> activities;
};

struct HarmonyConnectionStateChangedEvent {
    HarmonyConnectionState state;
};

// Marker only - handlers call Snapshot(), same shape as WeatherUpdatedEvent.
struct HarmonyConfigUpdatedEvent {};

// The Harmony module's connection manager - see ADR-0003/modules.md for
// the module contract this is the reference implementation of, and
// ADR-0029 for the protocol facts this is built against. Portable (built
// only on already-portable interfaces: HttpClient&, a WebSocketClient
// factory, Storage&, EventBus&), following OpenMeteoWeatherProvider's
// shape closely: a background Task owning its own connect/retry loop, a
// mutex-guarded Snapshot(), no LVGL dependency, fully host-testable.
//
// Scope for this pass: connect to a manually-configured hub address,
// fetch its device/activity list, and publish connection-state/config
// events. Sending IR commands, activities/devices screens, and a
// dashboard widget are separate, later M3 roadmap items - this class
// does not do any of that yet.
class HarmonyConnection : public Module {
public:
    using WebSocketClientFactory = std::function<std::unique_ptr<WebSocketClient>()>;

    // module_id "harmony" - see ADR-0012's per-module Storage namespacing.
    static constexpr char kModuleId[] = "harmony";
    static constexpr char kHubHostKey[] = "hub_host";

    // initial_backoff/max_backoff are injectable (defaulted to production
    // values) so tests can exercise the retry path without waiting out
    // the full 2s/60s backoff - the same "real default, test-overridable"
    // shape OpenMeteoWeatherProvider::poll_interval already uses.
    HarmonyConnection(HttpClient& http_client, WebSocketClientFactory make_websocket_client, Storage& storage,
                       EventBus& event_bus, std::chrono::milliseconds initial_backoff = std::chrono::seconds(2),
                       std::chrono::milliseconds max_backoff = std::chrono::seconds(60));

    // Module:
    void Start() override;
    void Stop() override;

    HarmonyConnectionSnapshot Snapshot() const;

    // Wakes the connection loop immediately - the Web UI's hub-address
    // save flow calls this (core/harmony_routes.h) so a newly-configured
    // or newly-changed address is tried right away instead of waiting out
    // whatever backoff delay is currently in progress. Same TriggerPoll()
    // shape as OpenMeteoWeatherProvider::TriggerPoll().
    void TriggerReconnect();

private:
    enum class WakeReason { kTimeout, kTriggered, kStopRequested };

    void ConnectionLoop(std::stop_token stop);
    // Runs the HTTP handshake, opens the WebSocket, and fetches the
    // device/activity config - true only if every step succeeded, in
    // which case ws_client_ is left open (owned by the calling thread)
    // and state_.devices/activities/has_config are already updated.
    bool ConnectAndFetchConfig(const std::string& hub_host);
    // Sends a lightweight read-only query over the already-open
    // ws_client_ and waits for any response, to detect a silently-dropped
    // connection between the coarser liveness-check interval - see
    // ADR-0029's Consequences on why ReceiveText() can't distinguish a
    // clean close from a timeout, and why this probe-based approach
    // works around that rather than needing it to.
    bool ProbeLiveness();
    void SetState(HarmonyConnectionState state);
    WakeReason Sleep(std::chrono::milliseconds delay, std::stop_token stop);

    HttpClient& http_client_;
    WebSocketClientFactory make_websocket_client_;
    Storage& storage_;
    EventBus& event_bus_;

    RetryBackoff backoff_;

    // Owned by, and only ever touched from, task_'s own thread - no mutex
    // needed for this member specifically, same single-owner reasoning
    // Task's own doc gives.
    std::unique_ptr<WebSocketClient> ws_client_;
    std::string hub_id_;  // activeRemoteId from the most recent handshake

    mutable std::mutex mutex_;
    HarmonyConnectionSnapshot state_;

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_requested_ = false;

    // Constructed by Start(), destroyed (stopped and joined) by Stop() -
    // see Module's own comment on why this class's Start()/Stop() control
    // a Task's lifetime directly rather than the task running from
    // construction the way OpenMeteoWeatherProvider's does.
    std::unique_ptr<Task> task_;
};

}  // namespace homedeck
