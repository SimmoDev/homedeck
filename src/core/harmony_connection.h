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
    // Empty until the first successful fetch (see FetchCurrentActivity())
    // - matches one of the ids in `activities` above (Harmony models "off"
    // as a regular activity, typically id "-1", not a separate concept).
    std::string current_activity_id;
};

struct HarmonyConnectionStateChangedEvent {
    HarmonyConnectionState state;
};

// Marker only - handlers call Snapshot(), same shape as WeatherUpdatedEvent.
struct HarmonyConfigUpdatedEvent {};

struct HarmonyCurrentActivityChangedEvent {
    std::string activity_id;
};

// The Harmony module's connection manager - see ADR-0003/modules.md for
// the module contract this is the reference implementation of, and
// ADR-0029 for the protocol facts this is built against. Portable (built
// only on already-portable interfaces: HttpClient&, a WebSocketClient
// factory, Storage&, EventBus&), following OpenMeteoWeatherProvider's
// shape closely: a background Task owning its own connect/retry loop, a
// mutex-guarded Snapshot(), no LVGL dependency, fully host-testable.
//
// Scope: connect to a manually-configured hub address, fetch its device/
// activity list and current activity, start an activity, and publish
// connection-state/config/current-activity events. Sending IR commands
// (Devices/Remote control) is a separate, later M3 roadmap item - this
// class does not do that yet.
//
// Current-activity freshness is best-effort, not push-driven: the hub's
// WS protocol can send unsolicited notifications, but this class's
// transport is a simple synchronous request/response loop (send one
// command, read exactly one reply) - push-message handling would be a
// substantially bigger, separate undertaking. Freshness instead comes
// from three points: right after connecting, right after StartActivity()
// sends its command, and the existing liveness-probe cycle otherwise -
// worst case kLivenessInterval (default 30s) stale if a hub-side change
// happens outside those triggers.
class HarmonyConnection : public Module {
public:
    using WebSocketClientFactory = std::function<std::unique_ptr<WebSocketClient>()>;

    // module_id "harmony" - see ADR-0012's per-module Storage namespacing.
    static constexpr char kModuleId[] = "harmony";
    static constexpr char kHubHostKey[] = "hub_host";

    // initial_backoff/max_backoff/liveness_interval are injectable
    // (defaulted to production values) so tests can exercise the retry
    // path and the periodic current-activity refresh without waiting out
    // the full 2s/60s backoff or the full 30s liveness interval - the
    // same "real default, test-overridable" shape
    // OpenMeteoWeatherProvider::poll_interval already uses.
    HarmonyConnection(HttpClient& http_client, WebSocketClientFactory make_websocket_client, Storage& storage,
                       EventBus& event_bus, std::chrono::milliseconds initial_backoff = std::chrono::seconds(2),
                       std::chrono::milliseconds max_backoff = std::chrono::seconds(60),
                       std::chrono::milliseconds liveness_interval = std::chrono::seconds(30));

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

    // Asks the connection loop to start this activity - safe to call
    // from any thread (e.g. ActivitiesScreen on the UI thread), unlike
    // ws_client_ itself, which only the connection loop's own thread ever
    // touches. A no-op if never connected: the request just waits
    // (harmlessly - see Sleep()'s watch_commands parameter) until a
    // connection exists to send it over.
    void StartActivity(const std::string& activity_id);

private:
    enum class WakeReason { kTimeout, kTriggered, kCommandPending, kStopRequested };

    void ConnectionLoop(std::stop_token stop);
    // Runs the HTTP handshake, opens the WebSocket, and fetches the
    // device/activity config - true only if every step succeeded, in
    // which case ws_client_ is left open (owned by the calling thread)
    // and state_.devices/activities/has_config are already updated.
    // Also calls FetchCurrentActivity() once, best-effort - its own
    // failure doesn't fail the connect, since the next liveness probe
    // will retry it.
    bool ConnectAndFetchConfig(const std::string& hub_host);
    // Sends getCurrentActivity over the already-open ws_client_, parses
    // `data.result`, and updates state_.current_activity_id + publishes
    // HarmonyCurrentActivityChangedEvent if it changed. Doubles as the
    // connected loop's liveness signal (false on any transport/parse
    // failure, including a silently-dropped connection) - see
    // ADR-0029's Consequences on why ReceiveText() can't distinguish a
    // clean close from a timeout, and why this probe-based approach
    // works around that rather than needing it to.
    bool FetchCurrentActivity();
    // Reads and clears any activity StartActivity() queued, sending it
    // over ws_client_ - called from the connected loop's own thread only.
    void SendPendingActivityCommand();
    void SetState(HarmonyConnectionState state);
    // watch_commands: only the connected loop's own wait should notice a
    // pending StartActivity() request (ws_client_ only exists then); the
    // unconfigured/error-backoff waits leave it queued rather than
    // waking early on it, since waking with nothing to send over would
    // otherwise busy-loop (the flag stays set until actually consumed).
    WakeReason Sleep(std::chrono::milliseconds delay, std::stop_token stop, bool watch_commands);

    HttpClient& http_client_;
    WebSocketClientFactory make_websocket_client_;
    Storage& storage_;
    EventBus& event_bus_;

    RetryBackoff backoff_;
    std::chrono::milliseconds liveness_interval_;

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
    // Guarded by wake_mutex_ too, not a separate mutex - StartActivity()
    // and TriggerReconnect() both just need to wake the same connection-
    // loop thread, so one wake channel is enough (see Sleep()'s
    // watch_commands parameter for how the loop tells the two apart).
    bool command_pending_ = false;
    std::string pending_activity_id_;

    // Constructed by Start(), destroyed (stopped and joined) by Stop() -
    // see Module's own comment on why this class's Start()/Stop() control
    // a Task's lifetime directly rather than the task running from
    // construction the way OpenMeteoWeatherProvider's does.
    std::unique_ptr<Task> task_;
};

}  // namespace homedeck
