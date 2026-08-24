#include "core/harmony_connection.h"

#include "core/json_request.h"
#include "third_party/nlohmann/json.hpp"

#include <cctype>

namespace homedeck {

namespace {

// setup.account?getProvisionInfo - the local handshake (see ADR-0029 for
// the protocol facts): POST straight to the hub's HTTP port, no
// authentication, returns the activeRemoteId this module needs for the
// WebSocket connection below. `id` is an arbitrary request-correlation
// number the hub echoes back - unused here, since a single in-flight
// request at a time never needs to match a reply to it.
constexpr char kProvisionInfoRequestBody[] = R"({"id":1,"cmd":"setup.account?getProvisionInfo","timeout":90000})";

// The hub's HTTP handshake endpoint rejects the request with a 400
// unless this exact Origin is present (see ADR-0029). Not an actual
// browser origin, just what the hub's own (presumably
// myharmony.com-web-app-derived) validation checks for.
constexpr char kHandshakeOrigin[] = "http://sl.dhg.myharmony.com";

constexpr int kConfigFetchTimeoutMs = 10000;
constexpr int kLivenessProbeTimeoutMs = 10000;
constexpr std::chrono::seconds kUnconfiguredRecheckInterval{5};

// Bounds DrainStaleMessages()'s own 0ms-poll loop - a distinct concern
// from HarmonyConnection::kMaxPendingCommands (the pending-command queue
// depth cap), which happens to share this value but isn't the thing this
// bound actually depends on. What this bound should track is
// FirmwareWebSocketClient::kMaxQueuedMessages (platform/firmware/
// websocket_client.cpp) - the most messages that backend's own receive
// queue can hold before it starts dropping the oldest itself, so this
// loop empties a full queue in one drain rather than leaving stale
// entries behind for the next one. Kept in sync by comment on both
// sides, not a shared symbol - the two live in different targets
// (homedeck_core vs. homedeck_platform_host) with no dependency edge
// between them for a constant this minor to justify introducing one.
constexpr size_t kMaxDrainIterations = 20;

// Parses `text` as JSON, discarded (same convention as a parse error) if
// it exceeds kMaxJsonNestingDepth (core/json_request.h) - every hub-response
// parse in this file goes through this rather than nlohmann::json::parse()
// directly, since every one of them ingests data from this protocol's
// unauthenticated transport (see ExceedsJsonNestingDepth()'s own comment).
nlohmann::json ParseBoundedJson(const std::string& text) {
    if (ExceedsJsonNestingDepth(text)) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

// Both build their URL by raw concatenation, not a URL builder - a
// `hub_host` containing `#`/`?`/`@` would otherwise change what the
// underlying URL parser (curl/esp_http_client) treats as the actual
// host/port (a fragment truncates the authority, `@` introduces
// userinfo, an extra `?` starts a second query string), silently
// redirecting the connection rather than just failing to reach it - see
// IsValidHubHost()'s own comment, which rejects all three for exactly
// this reason.
std::string HandshakeUrl(const std::string& hub_host) { return "http://" + hub_host + ":8088/"; }

std::string WebSocketUrl(const std::string& hub_host, const std::string& hub_id) {
    return "ws://" + hub_host + ":8088/?domain=svcs.myharmony.com&hubId=" + hub_id;
}

// The activeRemoteId field is a bare JSON number in the response this
// project's own live-hub probe observed, but every other Harmony-adjacent
// hbus command exchanges hubId as a string - normalizing to a string here
// once means every later WS message can just interpolate hub_id_ directly.
std::string NumberOrStringToString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    return "";
}

// data.activity[] entries carry "id"/"label" fields (see ADR-0029's
// Consequences). Entries missing either field are skipped rather than
// surfaced with a blank label. Devices need more than this (see
// ParseDevices() below), so this stays activity-only despite the name
// it had when both shared it.
template <typename Entry>
std::vector<Entry> ParseIdLabelArray(const nlohmann::json& array) {
    std::vector<Entry> result;
    if (!array.is_array()) return result;
    for (const auto& item : array) {
        if (!item.is_object()) continue;
        auto id_it = item.find("id");
        auto label_it = item.find("label");
        if (id_it == item.end() || label_it == item.end() || !label_it->is_string()) continue;
        // NumberOrStringToString() returns "" for a present-but-wrong-typed
        // id (null/bool/array/object) - skip rather than surface a
        // tappable entry whose id can never match anything (and whose
        // StartActivity("")/command send the hub has no defined behavior
        // for). This protocol has no authentication (see ADR-0029), so a
        // malformed field isn't purely hypothetical corruption.
        std::string id = NumberOrStringToString(*id_it);
        if (id.empty()) continue;
        result.push_back(Entry{std::move(id), label_it->get<std::string>()});
    }
    return result;
}

// data.device[].controlGroup[].function[] entries carry "name"/"label"/
// "action" (see ADR-0029's Consequences). `action` is already a ready-to-send
// JSON string (e.g. `{"command":"VolumeUp","type":"IRCommand","deviceId":"..."}`)
// - passed through as-is, not reconstructed from `name`/deviceId, since
// the hub's own copy is authoritative and this project has no reason to
// assume it could rebuild an equivalent string correctly for every
// command type.
std::vector<HarmonyControlGroup> ParseControlGroups(const nlohmann::json& array) {
    std::vector<HarmonyControlGroup> groups;
    if (!array.is_array()) return groups;
    for (const auto& group_item : array) {
        if (!group_item.is_object()) continue;
        auto name_it = group_item.find("name");
        if (name_it == group_item.end() || !name_it->is_string()) continue;

        HarmonyControlGroup group;
        group.name = name_it->get<std::string>();

        auto function_it = group_item.find("function");
        if (function_it != group_item.end() && function_it->is_array()) {
            for (const auto& function_item : *function_it) {
                if (!function_item.is_object()) continue;
                auto fn_name_it = function_item.find("name");
                auto fn_label_it = function_item.find("label");
                auto fn_action_it = function_item.find("action");
                if (fn_name_it == function_item.end() || !fn_name_it->is_string() ||
                    fn_label_it == function_item.end() || !fn_label_it->is_string() ||
                    fn_action_it == function_item.end() || !fn_action_it->is_string()) {
                    continue;
                }
                group.commands.push_back(HarmonyCommand{fn_name_it->get<std::string>(), fn_label_it->get<std::string>(),
                                                          fn_action_it->get<std::string>()});
            }
        }
        // A group with no (or no valid) commands is still kept - an empty
        // heading in the UI is harmless, and dropping it would silently
        // hide a group name the hub reported.
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<HarmonyDevice> ParseDevices(const nlohmann::json& array) {
    std::vector<HarmonyDevice> devices;
    if (!array.is_array()) return devices;
    for (const auto& item : array) {
        if (!item.is_object()) continue;
        auto id_it = item.find("id");
        auto label_it = item.find("label");
        if (id_it == item.end() || label_it == item.end() || !label_it->is_string()) continue;
        // See ParseIdLabelArray()'s identical check for why a
        // present-but-wrong-typed id is skipped rather than kept empty.
        std::string id = NumberOrStringToString(*id_it);
        if (id.empty()) continue;
        HarmonyDevice device;
        device.id = std::move(id);
        device.label = label_it->get<std::string>();
        device.control_groups = ParseControlGroups(item.value("controlGroup", nlohmann::json::array()));
        devices.push_back(std::move(device));
    }
    return devices;
}

}  // namespace

// activeRemoteId (see ConnectAndFetchConfig() below) becomes the trailing
// hubId value in WebSocketUrl()'s raw string concatenation, sourced from
// the hub's own handshake response - a protocol with no authentication
// at all (ADR-0029), unlike hub_host, which is admin-entered. Every
// activeRemoteId this project has observed is a bare decimal number;
// restricting to ASCII alphanumerics (rather than digits only) leaves
// room for a hub that returns e.g. a hex/UUID-shaped id without
// reopening the character classes - '&', '#', whitespace - that would
// let a malicious or spoofed response change what the connect URL's
// query string actually contains.
bool IsValidHubId(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) {
        if (!std::isalnum(c)) return false;
    }
    return true;
}

bool IsValidHubHost(const std::string& value) {
    if (value.find("://") != std::string::npos) {
        return false;
    }
    for (unsigned char c : value) {
        // Rejects ASCII whitespace, every other C0 control character and
        // DEL, and every non-ASCII byte outright - a hostname/IP has no
        // legitimate use for any of them, and it sidesteps having to
        // enumerate every multi-byte UTF-8 whitespace codepoint (e.g.
        // U+00A0 NBSP) the frontend's `/\s/` regex already rejects that a
        // byte-wise std::isspace() alone can't see. Mirrors
        // webui/src/lib/harmonyValidation.ts's control-character check.
        if (std::isspace(c) || c >= 0x80 || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    if (value.find('/') != std::string::npos) {
        return false;
    }
    // See HandshakeUrl()/WebSocketUrl()'s own comment on why these three
    // specifically, not just any non-hostname character.
    if (value.find_first_of("#?@") != std::string::npos) {
        return false;
    }
    // A bare (unbracketed) IPv6 literal, e.g. "::1" or "fe80::1", would
    // otherwise pass every check above and reach HandshakeUrl()/
    // WebSocketUrl()'s own raw concatenation - RFC 3986 requires an IPv6
    // literal to be bracketed in a URL authority, and this project has no
    // stated need for IPv6 hub addresses, so ':' is rejected outright
    // rather than adding bracket-aware parsing for a case nothing needs.
    if (value.find(':') != std::string::npos) {
        return false;
    }
    return true;
}

HarmonyConnection::HarmonyConnection(HttpClient& http_client, WebSocketClientFactory make_websocket_client,
                                      Storage& storage, EventBus& event_bus,
                                      std::chrono::milliseconds initial_backoff, std::chrono::milliseconds max_backoff,
                                      std::chrono::milliseconds liveness_interval,
                                      std::chrono::milliseconds max_pending_command_age)
    : http_client_(http_client),
      make_websocket_client_(std::move(make_websocket_client)),
      storage_(storage),
      event_bus_(event_bus),
      backoff_(initial_backoff, max_backoff),
      liveness_interval_(liveness_interval),
      max_pending_command_age_(max_pending_command_age) {}

void HarmonyConnection::Start() {
    if (task_) return;  // already running
    task_ = std::make_unique<Task>("harmony-connection", [this](std::stop_token stop) { ConnectionLoop(stop); });
}

void HarmonyConnection::Stop() {
    task_.reset();  // Task's destructor requests stop and joins
}

HarmonyConnectionSnapshot HarmonyConnection::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void HarmonyConnection::TriggerReconnect() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_requested_ = true;
    }
    wake_cv_.notify_one();
}

void HarmonyConnection::StartActivity(const std::string& activity_id) {
    EnqueueCommand(PendingCommand{activity_id, std::nullopt, std::chrono::steady_clock::now()});
}

void HarmonyConnection::PressDeviceCommand(const std::string& action) { EnqueueDeviceCommand(action, "press"); }

void HarmonyConnection::HoldDeviceCommand(const std::string& action) { EnqueueDeviceCommand(action, "hold"); }

void HarmonyConnection::ReleaseDeviceCommand(const std::string& action) { EnqueueDeviceCommand(action, "release"); }

void HarmonyConnection::EnqueueDeviceCommand(const std::string& action, const std::string& status) {
    EnqueueCommand(PendingCommand{std::nullopt, DeviceCommandRequest{action, status}, std::chrono::steady_clock::now()});
}

void HarmonyConnection::EnqueueCommand(PendingCommand command) {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        pending_commands_.push_back(std::move(command));
        while (pending_commands_.size() > kMaxPendingCommands) {
            pending_commands_.pop_front();
        }
    }
    wake_cv_.notify_one();
}

HarmonyConnection::WakeReason HarmonyConnection::Sleep(std::chrono::milliseconds delay, std::stop_token stop,
                                                        bool watch_commands) {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_cv_.wait_for(lock, delay, [this, &stop, watch_commands] {
        return wake_requested_ || (watch_commands && !pending_commands_.empty()) || stop.stop_requested();
    });
    if (stop.stop_requested()) {
        return WakeReason::kStopRequested;
    }
    if (wake_requested_) {
        wake_requested_ = false;
        return WakeReason::kTriggered;
    }
    if (watch_commands && !pending_commands_.empty()) {
        return WakeReason::kCommandPending;
    }
    return WakeReason::kTimeout;
}

void HarmonyConnection::SetState(HarmonyConnectionState state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.state = state;
    }
    // Published after mutex_ releases, not while held - see
    // OpenMeteoWeatherProvider::PollOnce()'s identical comment on why a
    // synchronous subscriber calling back into Snapshot() must not
    // self-deadlock on this same, non-recursive mutex_.
    event_bus_.Publish(HarmonyConnectionStateChangedEvent{state});
}

void HarmonyConnection::ClearConfigIfPresent(bool force_publish) {
    bool had_config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        had_config = state_.has_config;
        if (had_config) {
            state_.has_config = false;
            state_.devices.clear();
            state_.activities.clear();
            state_.current_activity_id.clear();
        }
    }
    if (had_config || force_publish) {
        event_bus_.Publish(HarmonyConfigUpdatedEvent{});
    }
}

void HarmonyConnection::ClearPendingCommandsIfAny() {
    bool had_commands;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        had_commands = !pending_commands_.empty();
        pending_commands_.clear();
    }
    if (had_commands) {
        event_bus_.Publish(HarmonyCommandDroppedEvent{});
    }
}

void HarmonyConnection::ConnectionLoop(std::stop_token stop) {
    // Woken on request_stop() too, not just by TriggerReconnect() or a
    // Sleep() timeout - preserves Task::~Task()'s "stops and joins
    // promptly" contract, same reasoning as
    // OpenMeteoWeatherProvider::PollLoop()'s identical stop_callback.
    std::stop_callback wake_on_stop(stop, [this] { wake_cv_.notify_one(); });

    while (!stop.stop_requested()) {
        std::optional<VersionedValue> hub_host_setting = storage_.GetSetting(kModuleId, kHubHostKey);
        if (!hub_host_setting.has_value() || hub_host_setting->value.empty()) {
            ClearConfigIfPresent();
            // Only once a previous hub was actually configured - a command
            // queued before any hub has ever been set (last_hub_host_ still
            // empty) has no prior hub to have been wrongly addressed to; it
            // stays queued, waiting for whichever hub is configured first,
            // same as StartActivity()'s own "no-op if never connected"
            // contract already documents.
            if (!last_hub_host_.empty()) {
                ClearPendingCommandsIfAny();
            }
            SetState(HarmonyConnectionState::kDisconnected);
            Sleep(kUnconfiguredRecheckInterval, stop, /*watch_commands=*/false);
            continue;
        }

        // A hub_host value change (not just clearing it, already handled
        // by the unconfigured branch above) means any currently-shown
        // devices/activities belong to a *different* hub, not the one
        // about to be (re)connected to - the "keep showing last-known
        // state through a transient drop" policy
        // (HarmonyConnectionSnapshot::has_config's own comment) only
        // makes sense for reconnecting to the *same* hub. Compared
        // before SetState(kConnecting) below so a Web UI status poll
        // racing this transition sees has_config already false rather
        // than a stale "connected" snapshot for the old hub - see
        // HarmonySettings.svelte's own post-save comment.
        if (hub_host_setting->value != last_hub_host_) {
            ClearConfigIfPresent(/*force_publish=*/true);
            // Same "only if a previous hub actually existed" guard as the
            // unconfigured branch above - last_hub_host_ empty here means
            // this is the very first connect attempt of the process, not a
            // switch away from a hub any queued command could have been
            // meant for.
            if (!last_hub_host_.empty()) {
                ClearPendingCommandsIfAny();
            }
        }
        last_hub_host_ = hub_host_setting->value;

        SetState(HarmonyConnectionState::kConnecting);
        if (!ConnectAndFetchConfig(hub_host_setting->value, stop)) {
            SetState(HarmonyConnectionState::kError);
            Sleep(backoff_.NextDelay(), stop, /*watch_commands=*/false);
            continue;
        }
        backoff_.ResetAttempts();
        SetState(HarmonyConnectionState::kConnected);
        event_bus_.Publish(HarmonyConfigUpdatedEvent{});

        // Any TriggerReconnect() call whose hub_host is still the one just
        // connected to - whether it landed before Start()'s background
        // thread even began running, or during the handshake/config-fetch
        // above - is already satisfied: the connection just established
        // already reflects it. Left set, it would instead be consumed by
        // the first Sleep() below, forcing one wholly unnecessary extra
        // reconnect cycle for a trigger that's already answered.
        //
        // But hub_host_setting (read once, at the top of this iteration)
        // can go stale during ConnectAndFetchConfig()'s own network round
        // trip (up to ~kConnectTimeoutMs + kConfigFetchTimeoutMs): a Web UI
        // save landing in that window is a genuine "the configured address
        // changed since I connected" request, not one this iteration's
        // connection already satisfies - discarding it here would strand
        // the connection on the address it just connected to, indefinitely
        // (a plain liveness timeout doesn't re-check storage; only another
        // explicit trigger or a connection drop does). storage_ is read
        // again here, under wake_mutex_ (so no concurrent TriggerReconnect()
        // call can land between this read and the reset below), to tell
        // the two cases apart: only discard the trigger if hub_host is
        // still what this iteration actually connected with.
        {
            std::lock_guard<std::mutex> lock(wake_mutex_);
            std::optional<VersionedValue> post_connect_hub_host = storage_.GetSetting(kModuleId, kHubHostKey);
            bool hub_host_unchanged =
                post_connect_hub_host.has_value() && post_connect_hub_host->value == hub_host_setting->value;
            if (hub_host_unchanged) {
                wake_requested_ = false;
            }
        }

        while (!stop.stop_requested()) {
            WakeReason reason = Sleep(liveness_interval_, stop, /*watch_commands=*/true);
            if (reason == WakeReason::kStopRequested) break;
            // Sleep() reports only one reason even though a trigger and a
            // pending command are independent conditions, not mutually
            // exclusive - draining here regardless of which one woke this
            // cycle means a manual reconnect request (kTriggered) never
            // strands an already-queued command until the next cycle
            // happens to catch it. A no-op when nothing is queued.
            if (!SendPendingCommands(stop)) break;  // connection dropped mid-send
            if (reason == WakeReason::kTriggered) break;  // re-check the configured address
            if (reason == WakeReason::kCommandPending) continue;  // stay connected - a command isn't a reconnect request
            if (FetchCurrentActivity(stop) != ActivityFetchResult::kOk) break;  // connection silently dropped, or unparseable
        }

        if (ws_client_) {
            ws_client_->Close();
            ws_client_.reset();
        }
    }

    // A command enqueued at the exact moment Stop() requests the stop
    // token races Sleep()'s predicate: stop_requested() takes priority
    // over kCommandPending, so the inner loop above can break without
    // ever reaching SendPendingCommands() for a command that was already
    // sitting in pending_commands_. Draining it here keeps every loss
    // path publishing HarmonyCommandDroppedEvent, not just the ones
    // SendPendingCommands() itself sees - one event for the batch,
    // matching its own "one event per batch, not one per dropped entry"
    // convention.
    bool dropped_on_shutdown;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        dropped_on_shutdown = !pending_commands_.empty();
        pending_commands_.clear();
    }
    if (dropped_on_shutdown) {
        event_bus_.Publish(HarmonyCommandDroppedEvent{});
    }

    SetState(HarmonyConnectionState::kDisconnected);
}

bool HarmonyConnection::ConnectAndFetchConfig(const std::string& hub_host, std::stop_token stop) {
    HttpClientResponse handshake =
        http_client_.Post(HandshakeUrl(hub_host), kProvisionInfoRequestBody, {{"Origin", kHandshakeOrigin}});
    if (!handshake.success || handshake.status_code != 200) {
        return false;
    }
    // Checked after each blocking call, not before it - a stop requested
    // while this thread was blocked has nothing left to interrupt mid-call
    // (see this method's own header comment), but there's no reason to
    // chain into the *next* blocking call once it's already known to be
    // pointless.
    if (stop.stop_requested()) return false;
    nlohmann::json parsed = ParseBoundedJson(handshake.body);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    auto data_it = parsed.find("data");
    if (data_it == parsed.end() || !data_it->is_object()) {
        return false;
    }
    auto remote_id_it = data_it->find("activeRemoteId");
    if (remote_id_it == data_it->end()) {
        return false;
    }
    hub_id_ = NumberOrStringToString(*remote_id_it);
    if (!IsValidHubId(hub_id_)) {
        hub_id_.clear();
        return false;
    }

    ws_client_ = make_websocket_client_();
    if (!ws_client_->Connect(WebSocketUrl(hub_host, hub_id_))) {
        ws_client_.reset();
        return false;
    }
    if (stop.stop_requested()) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }
    // The hub's WS protocol can send unsolicited notifications (see this
    // class's own header comment) - draining before the config request
    // below, the same reasoning FetchCurrentActivity() already applies
    // to its own send/receive, keeps a message pushed in the connect ->
    // config-request window from being misread as the config response
    // and failing this connect attempt spuriously.
    DrainStaleMessages();

    nlohmann::json config_request = {
        {"hubId", hub_id_},
        {"timeout", 30},
        {"hbus",
         {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?config"}, {"id", "0"}, {"params", nlohmann::json::object()}}},
    };
    if (!ws_client_->SendText(config_request.dump())) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }
    if (stop.stop_requested()) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }

    std::optional<std::string> response_text = ws_client_->ReceiveText(kConfigFetchTimeoutMs);
    if (!response_text.has_value()) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }

    nlohmann::json response = ParseBoundedJson(*response_text);
    if (response.is_discarded() || !response.is_object()) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }
    auto config_data_it = response.find("data");
    if (config_data_it == response.end() || !config_data_it->is_object()) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }

    std::vector<HarmonyDevice> devices = ParseDevices(config_data_it->value("device", nlohmann::json::array()));
    std::vector<HarmonyActivity> activities =
        ParseIdLabelArray<HarmonyActivity>(config_data_it->value("activity", nlohmann::json::array()));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.devices = std::move(devices);
        state_.activities = std::move(activities);
        state_.has_config = true;
    }
    // A parse-level failure here doesn't fail the connect (has_config is
    // already true above); the next liveness probe retries it, and the
    // connection itself is still known-healthy - see this class's own
    // header comment on the current-activity freshness trade-off. A
    // transport-level failure is different: it means the socket this
    // function just opened is already dead (e.g. the hub rebooted in the
    // narrow window between the config response above and this probe),
    // so reporting a successful connect anyway would leave the caller
    // believing kConnected for up to one full liveness_interval_ before
    // the next probe discovers it - fail the connect outright instead,
    // same as every earlier step in this function.
    if (FetchCurrentActivity(stop) == ActivityFetchResult::kTransportFailed) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }
    return true;
}

void HarmonyConnection::DrainStaleMessages() {
    if (!ws_client_) return;
    for (size_t i = 0; i < kMaxDrainIterations; ++i) {
        if (!ws_client_->ReceiveText(0).has_value()) break;
    }
}

HarmonyConnection::ActivityFetchResult HarmonyConnection::FetchCurrentActivity(std::stop_token stop) {
    if (!ws_client_) {
        return ActivityFetchResult::kTransportFailed;
    }
    DrainStaleMessages();
    nlohmann::json request = {
        {"hubId", hub_id_},
        {"timeout", 30},
        {"hbus",
         {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?getCurrentActivity"},
          {"id", "0"},
          {"params", {{"verb", "get"}}}}},
    };
    if (!ws_client_->SendText(request.dump())) {
        return ActivityFetchResult::kTransportFailed;
    }
    if (stop.stop_requested()) return ActivityFetchResult::kTransportFailed;
    std::optional<std::string> response_text = ws_client_->ReceiveText(kLivenessProbeTimeoutMs);
    if (!response_text.has_value()) {
        return ActivityFetchResult::kTransportFailed;
    }
    nlohmann::json response = ParseBoundedJson(*response_text);
    if (response.is_discarded() || !response.is_object()) {
        return ActivityFetchResult::kParseFailed;
    }
    auto data_it = response.find("data");
    if (data_it == response.end() || !data_it->is_object()) {
        return ActivityFetchResult::kParseFailed;
    }
    auto result_it = data_it->find("result");
    // A wrong-typed result (object/array/bool/null) isn't a valid activity
    // id either way - NumberOrStringToString() would silently coerce it to
    // "", which this class's own current_activity_id.empty() convention
    // reserves for "never successfully fetched," not "the hub sent
    // something unparseable." Treated as a failed probe, same as a
    // missing field.
    if (result_it == data_it->end() || !(result_it->is_string() || result_it->is_number_integer())) {
        return ActivityFetchResult::kParseFailed;
    }
    std::string activity_id = NumberOrStringToString(*result_it);

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.current_activity_id != activity_id) {
            state_.current_activity_id = activity_id;
            changed = true;
        }
    }
    // Published after mutex_ releases - see SetState()'s identical
    // reasoning.
    if (changed) {
        event_bus_.Publish(HarmonyCurrentActivityChangedEvent{activity_id});
    }
    return ActivityFetchResult::kOk;
}

bool HarmonyConnection::SendPendingCommands(std::stop_token stop) {
    std::deque<PendingCommand> commands;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        commands.swap(pending_commands_);
    }
    if (!ws_client_) {
        // shouldn't happen - only called from the connected inner loop -
        // but defensive regardless. commands was already swapped out of
        // pending_commands_ above, so silently returning here would lose
        // it with no signal at all, unlike every other loss path below
        // (stale-age drop, mid-batch send failure) - published even
        // though `commands` may be empty, since the caller (DevicesScreen/
        // ActivitiesScreen) can't tell that apart from "something really
        // was dropped" and either way nothing here got sent.
        if (!commands.empty()) {
            event_bus_.Publish(HarmonyCommandDroppedEvent{});
        }
        return false;
    }

    bool started_activity = false;
    bool dropped_stale_command = false;
    auto now = std::chrono::steady_clock::now();
    for (const PendingCommand& command : commands) {
        // See ConnectAndFetchConfig()'s own comment on why this is
        // checked between sends rather than attempting to interrupt one
        // mid-call - a stop requested partway through a batch skips the
        // rest of it rather than sending commands nobody can act on. This
        // command and everything still left in `commands` never reach
        // SendText() - same "one event per batch" convention as the
        // stale-age/send-failure drops below, not a silent loss Stop()
        // itself should be exempt from reporting.
        if (stop.stop_requested()) {
            event_bus_.Publish(HarmonyCommandDroppedEvent{});
            return false;
        }
        if (now - command.enqueued_at > max_pending_command_age_) {
            dropped_stale_command = true;  // see max_pending_command_age_'s own comment
            continue;
        }
        bool sent;
        if (command.activity_id) {
            nlohmann::json request = {
                {"hubId", hub_id_},
                {"timeout", 30},
                {"hbus",
                 {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?startactivity"},
                  {"id", "0"},
                  {"params",
                   {{"async", "true"},
                    {"timestamp", 0},
                    {"args", {{"rule", "start"}}},
                    {"activityId", *command.activity_id}}}}},
            };
            sent = ws_client_->SendText(request.dump());
            if (sent) started_activity = true;
        } else if (command.device_command) {
            sent = SendHoldAction(command.device_command->action, command.device_command->status);
        } else {
            continue;
        }
        // Stop at the first failed send rather than chasing it with more -
        // a dropped connection won't suddenly start succeeding mid-batch,
        // and every remaining entry in this batch would otherwise also
        // block on the same dead transport for no benefit. This entry and
        // every one still left in `commands` are dropped the same way a
        // stale entry is (see dropped_stale_command's own event below) -
        // without this, a long-press-repeat sequence whose connection
        // dies mid-batch loses its eventual ReleaseDeviceCommand silently,
        // with nothing telling DevicesScreen the command it just sent
        // never completed.
        if (!sent) {
            event_bus_.Publish(HarmonyCommandDroppedEvent{});
            return false;
        }
    }

    if (started_activity && !stop.stop_requested()) {
        // Best-effort immediate refresh - the hub's own activity switch is
        // itself asynchronous, so this may still read the old value; the
        // next liveness-probe cycle catches up regardless - see this
        // class's own header comment on the freshness trade-off.
        FetchCurrentActivity(stop);
    }
    // One event per batch, not one per dropped entry - see
    // HarmonyCommandDroppedEvent's own comment.
    if (dropped_stale_command) {
        event_bus_.Publish(HarmonyCommandDroppedEvent{});
    }
    return true;
}

bool HarmonyConnection::SendHoldAction(const std::string& action, const std::string& status) {
    nlohmann::json request = {
        {"hubId", hub_id_},
        {"timeout", 30},
        {"hbus",
         {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?holdAction"},
          {"id", "0"},
          {"params", {{"status", status}, {"timestamp", "0"}, {"verb", "render"}, {"action", action}}}}},
    };
    return ws_client_->SendText(request.dump());
}

}  // namespace homedeck
