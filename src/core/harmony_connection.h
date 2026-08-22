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
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

// Rejects a scheme prefix, embedded whitespace, a path, or a `#`/`?`/`@`
// character - every one of these would otherwise still build a URL
// (HandshakeUrl()/WebSocketUrl() below just concatenate this value in),
// but `#`/`?`/`@` specifically build a URL that connects somewhere other
// than the configured address (a fragment/query/userinfo component
// silently changing what the URL parser treats as the actual host/port),
// not just one that fails to connect the same opaque way an unreachable
// address does - see WebSocketUrl()'s own comment. Mirrors
// webui/src/lib/harmonyValidation.ts's hubHostError() exactly, so both
// surfaces reject the same values - this is the server-side half,
// invoked from POST /api/settings via core/settings_routes.h's generic
// SettingValidateFn (wired in ui/app_core.cpp), since that generic
// settings API - not a Harmony-specific endpoint - is what actually
// persists this value (see kHubHostKey below). Empty is accepted - it
// isn't this function's concern, since ConnectionLoop() already treats
// it as "not yet configured," not a malformed address.
bool IsValidHubHost(const std::string& value);

// One entry from a device's controlGroup[].function[] - a single sendable
// IR command. `action` is the hub's own ready-to-send JSON string (e.g.
// `{"command":"VolumeUp","type":"IRCommand","deviceId":"74494839"}`),
// passed through unchanged rather than reconstructed - see
// PressDeviceCommand()'s own comment.
struct HarmonyCommand {
    std::string name;
    std::string label;
    std::string action;
};

// One controlGroup - a named cluster of related commands (e.g. "Volume",
// "NavigationBasic") the hub itself groups commands into. Devices/Remote
// control (docs/roadmap.md's M3 section, ADR-0029's Consequences) render
// these groups directly rather than inventing a different grouping -
// "inputs" has no separate structure (it's just commands in a
// "Miscellaneous" group), and a device's own `Capabilities`/
// `powerFeatures` fields aren't rendered at all (empty on every device
// this project has seen - IR is one-way, nothing to poll).
struct HarmonyControlGroup {
    std::string name;
    std::vector<HarmonyCommand> commands;
};

struct HarmonyDevice {
    std::string id;
    std::string label;
    std::vector<HarmonyControlGroup> control_groups;
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

// Published when a queued command is dropped rather than reaching the hub,
// for any of three reasons: a command older than max_pending_command_age_
// (a connection outage outlasted the queue's own staleness bound, so it
// never reached the hub and never will); a send partway through
// SendPendingCommands()'s own batch failing, or that batch being
// interrupted by a stop request, either way taking every remaining queued
// command in the batch with it; or a command still sitting in the queue
// when ConnectionLoop() itself shuts down without ever reaching
// SendPendingCommands() at all (see ConnectionLoop()'s own post-loop
// drain). Marker only, same shape as HarmonyConfigUpdatedEvent - a UI
// showing an optimistic "in progress" status for a specific queued action
// (ActivitiesScreen's "Starting <name>...") is the only plausible
// subscriber, and it already tracks which action that was locally.
// Deliberately not published for EnqueueCommand()'s own cap-based
// drop-oldest (kMaxPendingCommands) - that's routine during a normal
// rapid-tap/long-hold burst, where older entries being superseded by
// newer ones isn't a failure worth surfacing.
struct HarmonyCommandDroppedEvent {};

// The Harmony module's connection manager - see ADR-0003/modules.md for
// the module contract this is the reference implementation of, and
// ADR-0029 for the protocol facts this is built against. Portable (built
// only on already-portable interfaces: HttpClient&, a WebSocketClient
// factory, Storage&, EventBus&), following OpenMeteoWeatherProvider's
// shape closely: a background Task owning its own connect/retry loop, a
// mutex-guarded Snapshot(), no LVGL dependency, fully host-testable.
//
// Scope: connect to a manually-configured hub address, fetch its device/
// activity list and current activity, start an activity, send a device's
// own IR commands (including sustained press-and-hold repeat while a
// command button stays held - CLAUDE.md's "long-press actions where
// supported"), and publish connection-state/config/current-activity
// events. PressDeviceCommand()/HoldDeviceCommand()/ReleaseDeviceCommand()
// map directly onto the hub's own three-state holdAction protocol
// (press/hold/release - see ADR-0029's Consequences); which of the three
// to send and when is a UI-layer decision driven by touch timing
// (DevicesScreen's own comment covers why), not something this class
// infers on its own.
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

    // initial_backoff/max_backoff/liveness_interval/max_pending_command_age
    // are injectable (defaulted to production values) so tests can
    // exercise the retry path and the periodic current-activity refresh
    // without waiting out the full 2s/60s backoff or the full 30s
    // liveness interval - the same "real default, test-overridable" shape
    // OpenMeteoWeatherProvider::poll_interval already uses.
    HarmonyConnection(HttpClient& http_client, WebSocketClientFactory make_websocket_client, Storage& storage,
                       EventBus& event_bus, std::chrono::milliseconds initial_backoff = std::chrono::seconds(2),
                       std::chrono::milliseconds max_backoff = std::chrono::seconds(60),
                       std::chrono::milliseconds liveness_interval = std::chrono::seconds(30),
                       std::chrono::milliseconds max_pending_command_age = std::chrono::seconds(5));

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

    // Sends one IR command's press/hold/release phase - `action` is a
    // device's own HarmonyCommand::action string, passed through as-is
    // (see that struct's own comment). Same any-thread-safe shape as
    // StartActivity(). All three exist because a caller (DevicesScreen)
    // needs to drive them independently as press/hold/release
    // happens - a single call that always sends a press+release pair
    // can't represent "still being held." A plain tap is just
    // PressDeviceCommand() immediately followed by ReleaseDeviceCommand().
    void PressDeviceCommand(const std::string& action);
    void HoldDeviceCommand(const std::string& action);
    void ReleaseDeviceCommand(const std::string& action);

private:
    // action/status for one queued holdAction send - status is "press",
    // "hold", or "release" (see PressDeviceCommand()/HoldDeviceCommand()/
    // ReleaseDeviceCommand()).
    struct DeviceCommandRequest {
        std::string action;
        std::string status;
    };

    // One queued, not-yet-sent command - raw intent only, not a pre-built
    // JSON payload. Building the actual payload needs hub_id_, which -
    // like ws_client_ - only the connection loop's own thread may touch
    // (see SendPendingCommands()); StartActivity()/PressDeviceCommand()/
    // HoldDeviceCommand()/ReleaseDeviceCommand() are called from other
    // threads (e.g. the UI thread), so they can't safely read hub_id_
    // themselves to pre-build it. Exactly one field is set per entry.
    // enqueued_at backs SendPendingCommands()'s staleness check below.
    struct PendingCommand {
        std::optional<std::string> activity_id;  // startactivity
        std::optional<DeviceCommandRequest> device_command;  // holdAction
        std::chrono::steady_clock::time_point enqueued_at;
    };

    // Bounds how many commands can accumulate while disconnected/
    // reconnecting - without a cap, a held remote button's own repeat
    // events (DevicesScreen's LONG_PRESSED_REPEAT handler) can queue
    // dozens of entries during even a brief drop, all replayed
    // back-to-back on reconnect. The oldest entry is dropped first once
    // full - most-recent intent matters more for a live remote.
    static constexpr size_t kMaxPendingCommands = 20;

    // Appends to pending_commands_ and enforces kMaxPendingCommands,
    // shared by StartActivity()/EnqueueDeviceCommand() below.
    void EnqueueCommand(PendingCommand command);

    enum class WakeReason { kTimeout, kTriggered, kCommandPending, kStopRequested };

    void ConnectionLoop(std::stop_token stop);
    // Runs the HTTP handshake, opens the WebSocket, and fetches the
    // device/activity config - true only if every step succeeded, in
    // which case ws_client_ is left open (owned by the calling thread)
    // and state_.devices/activities/has_config are already updated.
    // Also calls FetchCurrentActivity() once - a transport-level failure
    // (the socket just opened above is already dead, e.g. the hub
    // rebooted in the narrow window between the config response and this
    // probe) fails the whole connect attempt, the same as every earlier
    // step in this function; a parse-level failure doesn't, since the
    // next liveness probe will retry it and the connection itself is
    // still known-healthy. `stop` is checked between each blocking
    // network call (not mid-call - none of HttpClient/WebSocketClient
    // support cancelling an in-flight call) so a Stop() requested while
    // this is running doesn't needlessly chain through the rest of an
    // already-pointless sequence; each individual call still has its own
    // bounded timeout regardless.
    bool ConnectAndFetchConfig(const std::string& hub_host, std::stop_token stop);
    // FetchCurrentActivity()'s outcome, distinguishing *why* it failed -
    // ConnectAndFetchConfig()'s own best-effort call (see its comment)
    // needs to tell "the transport itself is dead" apart from "the hub
    // sent something this class doesn't understand," which a plain bool
    // can't express.
    enum class ActivityFetchResult { kOk, kTransportFailed, kParseFailed };
    // Sends getCurrentActivity over the already-open ws_client_, parses
    // `data.result`, and updates state_.current_activity_id + publishes
    // HarmonyCurrentActivityChangedEvent if it changed. Doubles as the
    // connected loop's liveness signal (non-kOk on any transport/parse
    // failure, including a silently-dropped connection - the loop itself
    // doesn't distinguish the two, only ConnectAndFetchConfig()'s
    // best-effort call does) - see ADR-0029's Consequences on why
    // ReceiveText() can't distinguish a clean close from a timeout, and
    // why this probe-based approach works around that rather than
    // needing it to. `stop` - see ConnectAndFetchConfig()'s own comment.
    ActivityFetchResult FetchCurrentActivity(std::stop_token stop);
    // Discards any already-buffered WS message on ws_client_ - called
    // right before FetchCurrentActivity() sends its own request. Every
    // SendHoldAction() call has no matching receive of its own (it can't
    // - a hold/release pair's "reply, if any" would otherwise need its
    // own dedicated request/response slot this class's simple
    // synchronous transport doesn't have - see this class's own header
    // comment on current-activity freshness); ADR-0029 leaves it an open
    // question whether the hub actually sends one. If it does, that
    // reply would otherwise sit unconsumed until the next ReceiveText()
    // call and get wrongly parsed as whatever that call actually
    // expects. Bounded by kMaxPendingCommands so a hub that keeps
    // sending unsolicited messages can't loop this forever - same cap
    // FirmwareWebSocketClient::message_queue_ itself uses. A 0ms
    // ReceiveText() is a non-blocking poll on both backends (see each
    // one's own implementation), not a wait, so this never adds latency
    // to a caller that has nothing to drain.
    void DrainStaleMessages();
    // Drains every PendingCommand StartActivity()/PressDeviceCommand()/
    // HoldDeviceCommand()/ReleaseDeviceCommand() queued, building and
    // sending each one's actual payload here (not in those methods
    // themselves - see PendingCommand's own comment on why) - called
    // from the connected loop's own thread only. Stops at the first
    // failed send rather than attempting the rest of the batch - a
    // dropped connection won't start succeeding mid-batch, so there's
    // nothing to gain from chasing it with more sends (each one able to
    // block on the same dead transport). One FetchCurrentActivity()
    // follow-up at the end if any successfully-sent entry was a
    // startactivity, not one per entry - a burst of queued commands only
    // needs one refresh. Returns false if any send failed (a dropped
    // connection mid-batch) - the caller treats that the same as a
    // failed liveness probe rather than silently discarding it, since a
    // silent send failure would otherwise strand the remaining queued
    // commands with no feedback and no retry until the next
    // liveness_interval_. `stop` - see ConnectAndFetchConfig()'s own
    // comment; checked between each queued command's own send.
    bool SendPendingCommands(std::stop_token stop);
    // Enqueues a device command with the given status - the shared body
    // of PressDeviceCommand()/HoldDeviceCommand()/ReleaseDeviceCommand(),
    // which differ only in which status they pass.
    void EnqueueDeviceCommand(const std::string& action, const std::string& status);
    // One holdAction message. Returns SendText()'s own result.
    bool SendHoldAction(const std::string& action, const std::string& status);
    void SetState(HarmonyConnectionState state);
    // Called from the "hub_host is unconfigured" branch of ConnectionLoop()
    // - clears devices/activities/current_activity_id and has_config back
    // to their construction-time defaults, and publishes
    // HarmonyConfigUpdatedEvent so ActivitiesScreen/DevicesScreen (which
    // key their entire rendered list on has_config) drop the previous
    // hub's now-meaningless data instead of continuing to show it as if
    // still live. A no-op (and doesn't publish) if has_config is already
    // false - keeps the unconfigured branch's own 5s recheck loop from
    // publishing the event every cycle once already cleared. Deliberately
    // not shared with the transient disconnect/error paths - those keep
    // showing the last-known state on purpose (see
    // HarmonyConnectionSnapshot::has_config's own comment); this is only
    // for the "user explicitly removed the hub" case, which has no
    // "the hub might come back on its own" reasoning to preserve.
    //
    // force_publish: the "hub_host changed to a different address" branch
    // of ConnectionLoop() passes true - unlike the unconfigured branch, it
    // needs HarmonyConfigUpdatedEvent to fire even when has_config was
    // already false (e.g. the previous address never successfully
    // connected either), since HarmonyNotificationBridge subscribes to
    // this event specifically to tell "a fresh address is being tried" -
    // worth a new failure notification - apart from "the same address is
    // just retrying after backoff" - not. See that class's own comment.
    void ClearConfigIfPresent(bool force_publish = false);
    // watch_commands: only the connected loop's own wait should notice a
    // pending command (ws_client_ only exists then); the unconfigured/
    // error-backoff waits leave it queued rather than waking early on
    // it, since waking with nothing to send over would otherwise
    // busy-loop (entries stay queued until actually consumed).
    WakeReason Sleep(std::chrono::milliseconds delay, std::stop_token stop, bool watch_commands);

    HttpClient& http_client_;
    WebSocketClientFactory make_websocket_client_;
    Storage& storage_;
    EventBus& event_bus_;

    RetryBackoff backoff_;
    std::chrono::milliseconds liveness_interval_;
    // SendPendingCommands() drops any entry older than this rather than
    // sending it - a queued command from long enough ago no longer
    // reflects what the user actually wants sent to a hub whose
    // real-world state has moved on.
    std::chrono::milliseconds max_pending_command_age_;

    // Owned by, and only ever touched from, task_'s own thread - no mutex
    // needed for this member specifically, same single-owner reasoning
    // Task's own doc gives.
    std::unique_ptr<WebSocketClient> ws_client_;
    std::string hub_id_;  // activeRemoteId from the most recent handshake
    // The hub_host value ConnectionLoop() last attempted (or connected
    // to) - compared on every outer-loop pass to tell "reconnecting to
    // the same hub" apart from "the user pointed this at a different
    // hub," see ConnectionLoop()'s own comment. Empty at construction,
    // matching state_.has_config's own default-false (nothing to clear
    // on the very first connect attempt either way).
    std::string last_hub_host_;

    mutable std::mutex mutex_;
    HarmonyConnectionSnapshot state_;

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_requested_ = false;
    // Guarded by wake_mutex_ too, not a separate mutex - StartActivity()/
    // Press/Hold/ReleaseDeviceCommand() and TriggerReconnect() both just
    // need to wake the same connection-loop thread, so one wake channel
    // is enough (see Sleep()'s watch_commands parameter for how the loop
    // tells them apart).
    std::deque<PendingCommand> pending_commands_;

    // Constructed by Start(), destroyed (stopped and joined) by Stop() -
    // see Module's own comment on why this class's Start()/Stop() control
    // a Task's lifetime directly rather than the task running from
    // construction the way OpenMeteoWeatherProvider's does.
    std::unique_ptr<Task> task_;
};

}  // namespace homedeck
