#include "core/harmony_connection.h"

#include "third_party/nlohmann/json.hpp"

namespace homedeck {

namespace {

// setup.account?getProvisionInfo - the local handshake, verified live
// against the reference hub during this feature's own design pass (see
// ADR-0029): POST straight to the hub's HTTP port, no authentication,
// returns the activeRemoteId this module needs for the WebSocket
// connection below. `id` is an arbitrary request-correlation number the
// hub echoes back - unused here, since a single in-flight request at a
// time never needs to match a reply to it.
constexpr char kProvisionInfoRequestBody[] = R"({"id":1,"cmd":"setup.account?getProvisionInfo","timeout":90000})";

// The hub's HTTP handshake endpoint rejects the request with a 400
// unless this exact Origin is present - confirmed against the reference
// hub during this feature's own design and verification passes (see
// ADR-0029). Not an actual browser origin, just what the hub's own
// (presumably myharmony.com-web-app-derived) validation checks for.
constexpr char kHandshakeOrigin[] = "http://sl.dhg.myharmony.com";

constexpr int kConfigFetchTimeoutMs = 10000;
constexpr int kLivenessProbeTimeoutMs = 10000;
constexpr std::chrono::seconds kUnconfiguredRecheckInterval{5};

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

// data.activity[] entries carry "id"/"label" fields - confirmed against
// the reference hub's actual config payload (see ADR-0029's
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
        result.push_back(Entry{NumberOrStringToString(*id_it), label_it->get<std::string>()});
    }
    return result;
}

// data.device[].controlGroup[].function[] entries carry "name"/"label"/
// "action" - confirmed against the reference hub's actual config payload
// (see ADR-0029's Consequences). `action` is already a ready-to-send
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
        // hide a real group name the hub reported.
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
        HarmonyDevice device;
        device.id = NumberOrStringToString(*id_it);
        device.label = label_it->get<std::string>();
        device.control_groups = ParseControlGroups(item.value("controlGroup", nlohmann::json::array()));
        devices.push_back(std::move(device));
    }
    return devices;
}

}  // namespace

HarmonyConnection::HarmonyConnection(HttpClient& http_client, WebSocketClientFactory make_websocket_client,
                                      Storage& storage, EventBus& event_bus,
                                      std::chrono::milliseconds initial_backoff, std::chrono::milliseconds max_backoff,
                                      std::chrono::milliseconds liveness_interval)
    : http_client_(http_client),
      make_websocket_client_(std::move(make_websocket_client)),
      storage_(storage),
      event_bus_(event_bus),
      backoff_(initial_backoff, max_backoff),
      liveness_interval_(liveness_interval) {}

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
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        pending_commands_.push_back(PendingCommand{activity_id, std::nullopt});
    }
    wake_cv_.notify_one();
}

void HarmonyConnection::SendDeviceCommand(const std::string& action) {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        pending_commands_.push_back(PendingCommand{std::nullopt, action});
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

void HarmonyConnection::ConnectionLoop(std::stop_token stop) {
    // Woken on request_stop() too, not just by TriggerReconnect() or a
    // Sleep() timeout - preserves Task::~Task()'s "stops and joins
    // promptly" contract, same reasoning as
    // OpenMeteoWeatherProvider::PollLoop()'s identical stop_callback.
    std::stop_callback wake_on_stop(stop, [this] { wake_cv_.notify_one(); });

    while (!stop.stop_requested()) {
        std::optional<VersionedValue> hub_host_setting = storage_.GetSetting(kModuleId, kHubHostKey);
        if (!hub_host_setting.has_value() || hub_host_setting->value.empty()) {
            SetState(HarmonyConnectionState::kDisconnected);
            Sleep(kUnconfiguredRecheckInterval, stop, /*watch_commands=*/false);
            continue;
        }

        SetState(HarmonyConnectionState::kConnecting);
        if (!ConnectAndFetchConfig(hub_host_setting->value)) {
            SetState(HarmonyConnectionState::kError);
            Sleep(backoff_.NextDelay(), stop, /*watch_commands=*/false);
            continue;
        }
        backoff_.ResetAttempts();
        SetState(HarmonyConnectionState::kConnected);
        event_bus_.Publish(HarmonyConfigUpdatedEvent{});

        while (!stop.stop_requested()) {
            WakeReason reason = Sleep(liveness_interval_, stop, /*watch_commands=*/true);
            if (reason == WakeReason::kStopRequested) break;
            if (reason == WakeReason::kTriggered) break;  // re-check the configured address
            if (reason == WakeReason::kCommandPending) {
                SendPendingCommands();
                continue;  // stay connected - a command isn't a reconnect request
            }
            if (!FetchCurrentActivity()) break;  // connection silently dropped
        }

        if (ws_client_) {
            ws_client_->Close();
            ws_client_.reset();
        }
    }

    SetState(HarmonyConnectionState::kDisconnected);
}

bool HarmonyConnection::ConnectAndFetchConfig(const std::string& hub_host) {
    HttpClientResponse handshake =
        http_client_.Post(HandshakeUrl(hub_host), kProvisionInfoRequestBody, {{"Origin", kHandshakeOrigin}});
    if (!handshake.success || handshake.status_code != 200) {
        return false;
    }
    nlohmann::json parsed = nlohmann::json::parse(handshake.body, nullptr, /*allow_exceptions=*/false);
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
    if (hub_id_.empty()) {
        return false;
    }

    ws_client_ = make_websocket_client_();
    if (!ws_client_->Connect(WebSocketUrl(hub_host, hub_id_))) {
        ws_client_.reset();
        return false;
    }

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

    std::optional<std::string> response_text = ws_client_->ReceiveText(kConfigFetchTimeoutMs);
    if (!response_text.has_value()) {
        ws_client_->Close();
        ws_client_.reset();
        return false;
    }

    nlohmann::json response = nlohmann::json::parse(*response_text, nullptr, /*allow_exceptions=*/false);
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
    // Best-effort - its own failure doesn't fail the connect (has_config
    // is already true above); the next liveness probe retries it. See
    // this class's own header comment on the current-activity freshness
    // trade-off.
    FetchCurrentActivity();
    return true;
}

bool HarmonyConnection::FetchCurrentActivity() {
    if (!ws_client_) {
        return false;
    }
    nlohmann::json request = {
        {"hubId", hub_id_},
        {"timeout", 30},
        {"hbus",
         {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?getCurrentActivity"},
          {"id", "0"},
          {"params", {{"verb", "get"}}}}},
    };
    if (!ws_client_->SendText(request.dump())) {
        return false;
    }
    std::optional<std::string> response_text = ws_client_->ReceiveText(kLivenessProbeTimeoutMs);
    if (!response_text.has_value()) {
        return false;
    }
    nlohmann::json response = nlohmann::json::parse(*response_text, nullptr, /*allow_exceptions=*/false);
    if (response.is_discarded() || !response.is_object()) {
        return false;
    }
    auto data_it = response.find("data");
    if (data_it == response.end() || !data_it->is_object()) {
        return false;
    }
    auto result_it = data_it->find("result");
    if (result_it == data_it->end()) {
        return false;
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
    return true;
}

void HarmonyConnection::SendPendingCommands() {
    std::deque<PendingCommand> commands;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        commands.swap(pending_commands_);
    }
    if (!ws_client_) {
        return;  // shouldn't happen - only called from the connected inner loop - but defensive regardless
    }

    bool started_activity = false;
    for (const PendingCommand& command : commands) {
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
            ws_client_->SendText(request.dump());
            started_activity = true;
        } else if (command.device_action) {
            SendHoldAction(*command.device_action, "press");
            SendHoldAction(*command.device_action, "release");
        }
    }

    if (started_activity) {
        // Best-effort immediate refresh - the hub's own activity switch is
        // itself asynchronous, so this may still read the old value; the
        // next liveness-probe cycle catches up regardless - see this
        // class's own header comment on the freshness trade-off.
        FetchCurrentActivity();
    }
}

void HarmonyConnection::SendHoldAction(const std::string& action, const std::string& status) {
    nlohmann::json request = {
        {"hubId", hub_id_},
        {"timeout", 30},
        {"hbus",
         {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?holdAction"},
          {"id", "0"},
          {"params", {{"status", status}, {"timestamp", "0"}, {"verb", "render"}, {"action", action}}}}},
    };
    ws_client_->SendText(request.dump());
}

}  // namespace homedeck
