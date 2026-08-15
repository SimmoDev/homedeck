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

// data.device[]/data.activity[] entries are expected to carry "id"/
// "label" fields per the community-documented config schema (aioharmony,
// the Home Assistant pyharmony websockets branch) - not yet confirmed
// against the reference hub's actual config payload the way the
// handshake step above was (see ADR-0029's Consequences). Entries
// missing either field are skipped rather than surfaced with a blank
// label.
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
        pending_activity_id_ = activity_id;
        command_pending_ = true;
    }
    wake_cv_.notify_one();
}

HarmonyConnection::WakeReason HarmonyConnection::Sleep(std::chrono::milliseconds delay, std::stop_token stop,
                                                        bool watch_commands) {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_cv_.wait_for(lock, delay, [this, &stop, watch_commands] {
        return wake_requested_ || (watch_commands && command_pending_) || stop.stop_requested();
    });
    if (stop.stop_requested()) {
        return WakeReason::kStopRequested;
    }
    if (wake_requested_) {
        wake_requested_ = false;
        return WakeReason::kTriggered;
    }
    if (watch_commands && command_pending_) {
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
                SendPendingActivityCommand();
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

    std::vector<HarmonyDevice> devices = ParseIdLabelArray<HarmonyDevice>(config_data_it->value("device", nlohmann::json::array()));
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

void HarmonyConnection::SendPendingActivityCommand() {
    std::string activity_id;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        if (!command_pending_) {
            return;
        }
        activity_id = pending_activity_id_;
        command_pending_ = false;
        pending_activity_id_.clear();
    }
    if (!ws_client_) {
        return;  // shouldn't happen - only called from the connected inner loop - but defensive regardless
    }

    nlohmann::json request = {
        {"hubId", hub_id_},
        {"timeout", 30},
        {"hbus",
         {{"cmd", "vnd.logitech.harmony/vnd.logitech.harmony.engine?startactivity"},
          {"id", "0"},
          {"params",
           {{"async", "true"}, {"timestamp", 0}, {"args", {{"rule", "start"}}}, {"activityId", activity_id}}}}},
    };
    ws_client_->SendText(request.dump());
    // Best-effort immediate refresh - the hub's own activity switch is
    // itself asynchronous, so this may still read the old value; the
    // next liveness-probe cycle catches up regardless - see this class's
    // own header comment on the freshness trade-off.
    FetchCurrentActivity();
}

}  // namespace homedeck
