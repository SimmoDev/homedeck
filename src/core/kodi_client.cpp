#include "core/kodi_client.h"

#include "core/json_request.h"
#include "third_party/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>

namespace homedeck {

namespace {

constexpr int kCallTimeoutMs = 8000;
constexpr std::chrono::seconds kNoTargetRecheckInterval{5};

// Bounds PumpNotifications()'s own non-blocking drain loop - the same
// role kMaxDrainIterations plays in harmony_connection.cpp. A hub/box
// that keeps a client's receive buffer permanently full can't turn one
// drain into an unbounded loop.
constexpr size_t kMaxPumpIterations = 32;

// See harmony_connection.cpp's ParseBoundedJson() - every frame here
// also comes off an unauthenticated LAN transport (ADR-0030).
nlohmann::json ParseBoundedJson(const std::string& text) {
    if (ExceedsJsonNestingDepth(text)) {
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }
    return nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
}

std::string WebSocketUrl(const std::string& host, uint16_t port) {
    // An IPv6 literal (from a discovery result's resolved address) must
    // be bracketed in a URL authority - IsValidKodiHost() rejects ':'
    // for the manual-override path, but a discovered address bypasses
    // that check.
    std::string authority = host;
    if (authority.find(':') != std::string::npos && !authority.empty() && authority.front() != '[') {
        authority = "[" + authority + "]";
    }
    return "ws://" + authority + ":" + std::to_string(port) + "/jsonrpc";
}

long long MillisFromTimeObject(const nlohmann::json& t) {
    if (!t.is_object()) {
        return 0;
    }
    long long seconds = t.value("hours", 0) * 3600LL + t.value("minutes", 0) * 60LL + t.value("seconds", 0);
    return seconds * 1000LL + t.value("milliseconds", 0);
}

KodiPlaybackState PlaybackFromSpeed(int speed) {
    return speed == 0 ? KodiPlaybackState::kPaused : KodiPlaybackState::kPlaying;
}

// Pulls title/show/season/episode/type out of a notification's or a
// Player.GetItem response's `item` object. Missing/blank fields are left
// at their struct defaults so a later, better-populated source (or the
// notification, for add-on playback - see ADR-0030) can fill them.
void ApplyItemFields(const nlohmann::json& item, KodiNowPlaying& now_playing) {
    if (!item.is_object()) {
        return;
    }
    std::string title = item.value("title", "");
    if (title.empty()) {
        title = item.value("label", "");  // add-on playback: `title` blank, `label` usable
    }
    if (!title.empty()) {
        now_playing.title = title;
    }
    if (item.contains("showtitle") && item["showtitle"].is_string() && !item["showtitle"].get<std::string>().empty()) {
        now_playing.show_title = item["showtitle"].get<std::string>();
    }
    if (item.contains("season") && item["season"].is_number_integer()) {
        int season = item["season"].get<int>();
        if (season >= 0) {
            now_playing.season = season;
        }
    }
    if (item.contains("episode") && item["episode"].is_number_integer()) {
        int episode = item["episode"].get<int>();
        if (episode >= 0) {
            now_playing.episode = episode;
        }
    }
    if (item.contains("type") && item["type"].is_string()) {
        std::string type = item["type"].get<std::string>();
        if (!type.empty() && type != "unknown") {
            now_playing.media_type = type;
        }
    }
}

}  // namespace

bool IsValidKodiHost(const std::string& value) {
    if (value.find("://") != std::string::npos) {
        return false;
    }
    for (unsigned char c : value) {
        // Same byte class IsValidHubHost() rejects, and for the same
        // reasons - see harmony_connection.cpp's own comment.
        if (std::isspace(c) || c >= 0x80 || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    if (value.find('/') != std::string::npos) {
        return false;
    }
    if (value.find_first_of("#?@") != std::string::npos) {
        return false;
    }
    // A bare (unbracketed) IPv6 literal would reach WebSocketUrl()'s raw
    // concatenation - rejected here for the manual-override path exactly
    // as IsValidHubHost() does; a discovered address is bracketed by
    // WebSocketUrl() itself instead.
    if (value.find(':') != std::string::npos) {
        return false;
    }
    return true;
}

KodiClient::KodiClient(WebSocketClientFactory make_websocket_client, MdnsBrowser& mdns_browser, Storage& storage,
                       EventBus& event_bus, std::chrono::milliseconds initial_backoff,
                       std::chrono::milliseconds max_backoff, std::chrono::milliseconds reconcile_interval,
                       std::chrono::milliseconds pump_interval, std::chrono::milliseconds browse_timeout,
                       std::chrono::milliseconds max_pending_command_age)
    : make_websocket_client_(std::move(make_websocket_client)),
      mdns_browser_(mdns_browser),
      storage_(storage),
      event_bus_(event_bus),
      backoff_(initial_backoff, max_backoff),
      reconcile_interval_(reconcile_interval),
      pump_interval_(pump_interval),
      browse_timeout_(browse_timeout),
      max_pending_command_age_(max_pending_command_age) {}

void KodiClient::Start() {
    if (task_) {
        return;
    }
    task_ = std::make_unique<Task>("kodi-client", [this](std::stop_token stop) { ConnectionLoop(stop); });
}

void KodiClient::Stop() {
    task_.reset();  // Task's destructor requests stop and joins
}

KodiSnapshot KodiClient::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void KodiClient::TriggerReconnect() {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        wake_requested_ = true;
    }
    wake_cv_.notify_one();
}

void KodiClient::SetState(KodiConnectionState state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.state = state;
    }
    // Published after mutex_ releases - a synchronous subscriber calling
    // back into Snapshot() must not self-deadlock on the non-recursive
    // mutex_ (see HarmonyConnection::SetState()'s identical note).
    event_bus_.Publish(KodiConnectionStateChangedEvent{state});
}

KodiClient::WakeReason KodiClient::Sleep(std::chrono::milliseconds delay, std::stop_token stop, bool watch_commands) {
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

std::optional<KodiClient::Target> KodiClient::ResolveTarget() {
    std::optional<VersionedValue> host_setting = storage_.GetSetting(kModuleId, kHostKey);
    if (host_setting.has_value() && !host_setting->value.empty() && IsValidKodiHost(host_setting->value)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.resolved_host = host_setting->value;
            state_.discovered.clear();
        }
        return Target{host_setting->value, kDefaultPort};
    }

    std::vector<MdnsService> instances = mdns_browser_.Browse(kServiceType, browse_timeout_);
    // Keep only instances we could actually connect to.
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                   [](const MdnsService& s) {
                                       return (s.address.empty() && s.hostname.empty()) || s.port == 0;
                                   }),
                    instances.end());

    std::optional<VersionedValue> uuid_setting = storage_.GetSetting(kModuleId, kInstanceUuidKey);
    const std::string selected_uuid =
        uuid_setting.has_value() ? uuid_setting->value : std::string();

    const MdnsService* chosen = nullptr;
    if (!selected_uuid.empty()) {
        for (const MdnsService& s : instances) {
            auto it = s.txt.find("uuid");
            if (it != s.txt.end() && it->second == selected_uuid) {
                chosen = &s;
                break;
            }
        }
    } else if (instances.size() == 1) {
        chosen = &instances.front();  // auto-select the only instance (ADR-0030)
    }
    // else: nothing discovered, or >1 with no saved selection - the
    // "ask the user to choose in settings" case; leave chosen null.

    std::string resolved_host;
    std::optional<Target> target;
    if (chosen != nullptr) {
        const std::string& host = !chosen->address.empty() ? chosen->address : chosen->hostname;
        target = Target{host, chosen->port};
        resolved_host = host + ":" + std::to_string(chosen->port);
    }

    std::vector<KodiDiscoveredInstance> discovered;
    discovered.reserve(instances.size());
    for (const MdnsService& s : instances) {
        auto uuid_it = s.txt.find("uuid");
        discovered.push_back(KodiDiscoveredInstance{
            s.instance_name, !s.address.empty() ? s.address : s.hostname,
            uuid_it != s.txt.end() ? uuid_it->second : std::string()});
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.discovered = std::move(discovered);
        state_.resolved_host = resolved_host;
    }
    return target;
}

void KodiClient::ConnectionLoop(std::stop_token stop) {
    std::stop_callback wake_on_stop(stop, [this] { wake_cv_.notify_one(); });

    while (!stop.stop_requested()) {
        std::optional<Target> target = ResolveTarget();
        if (!target.has_value()) {
            SetState(KodiConnectionState::kDisconnected);
            Sleep(kNoTargetRecheckInterval, stop, /*watch_commands=*/false);
            continue;
        }

        SetState(KodiConnectionState::kConnecting);
        if (!ConnectAndPrime(*target, stop)) {
            if (ws_client_) {
                ws_client_->Close();
                ws_client_.reset();
            }
            SetState(KodiConnectionState::kError);
            Sleep(backoff_.NextDelay(), stop, /*watch_commands=*/false);
            continue;
        }
        backoff_.ResetAttempts();
        SetState(KodiConnectionState::kConnected);

        auto last_reconcile = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            WakeReason reason = Sleep(pump_interval_, stop, /*watch_commands=*/true);
            if (reason == WakeReason::kStopRequested) {
                break;
            }
            PumpNotifications();
            // Drained regardless of wake reason - a reconnect trigger or
            // a reconcile timeout must not strand an already-queued
            // command until the next cycle. A no-op when nothing is
            // queued (same as HarmonyConnection's loop).
            if (!SendPendingCommands(stop)) {
                break;  // transport dropped mid-send
            }
            if (reason == WakeReason::kTriggered) {
                break;  // re-resolve the target - host/instance selection may have changed
            }
            if (reason == WakeReason::kCommandPending) {
                continue;  // stay connected - a command isn't a reconnect request
            }
            auto now = std::chrono::steady_clock::now();
            if (needs_immediate_poll_ || now - last_reconcile >= reconcile_interval_) {
                needs_immediate_poll_ = false;
                if (!ReconcilePoll(stop)) {
                    break;  // transport dead
                }
                last_reconcile = std::chrono::steady_clock::now();
            }
        }

        if (ws_client_) {
            ws_client_->Close();
            ws_client_.reset();
        }
    }

    SetState(KodiConnectionState::kDisconnected);
}

bool KodiClient::ConnectAndPrime(const Target& target, std::stop_token stop) {
    ws_client_ = make_websocket_client_();
    if (!ws_client_->Connect(WebSocketUrl(target.host, target.port))) {
        return false;
    }
    if (stop.stop_requested()) {
        return false;
    }
    // One reconcile poll up front so a client that connects while
    // something is already playing shows it immediately, rather than
    // blank until the first pushed notification. Doubles as a check
    // that the socket is actually alive.
    return ReconcilePoll(stop);
}

void KodiClient::PumpNotifications() {
    if (!ws_client_) {
        return;
    }
    for (size_t i = 0; i < kMaxPumpIterations; ++i) {
        std::optional<std::string> text = ws_client_->ReceiveText(0);
        if (!text.has_value()) {
            break;
        }
        HandleNotification(*text);
    }
}

std::optional<std::string> KodiClient::Call(const std::string& method, const std::string& params_json, int timeout_ms,
                                            std::stop_token stop) {
    if (!ws_client_) {
        return std::nullopt;
    }
    const int id = ++next_rpc_id_;
    nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params_json.empty()) {
        nlohmann::json params = ParseBoundedJson(params_json);
        if (!params.is_discarded()) {
            request["params"] = std::move(params);
        }
    }
    if (!ws_client_->SendText(request.dump())) {
        return std::nullopt;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return std::nullopt;
        }
        const int remaining =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        std::optional<std::string> text = ws_client_->ReceiveText(std::max(remaining, 1));
        if (!text.has_value()) {
            return std::nullopt;  // timeout / closed / error - transport is done
        }
        nlohmann::json frame = ParseBoundedJson(*text);
        if (frame.is_discarded() || !frame.is_object()) {
            continue;  // unparseable frame - ignore, keep waiting for ours
        }
        auto id_it = frame.find("id");
        if (id_it != frame.end() && id_it->is_number_integer() && id_it->get<int>() == id) {
            return text;  // our response
        }
        if (frame.contains("method")) {
            HandleNotification(*text);  // an interleaved pushed notification
        }
        // else: a response to a different id (e.g. a fire-and-forget
        // command's reply) - discard and keep waiting for ours.
    }
    return std::nullopt;
}

bool KodiClient::ReconcilePoll(std::stop_token stop) {
    std::optional<std::string> app_text =
        Call("Application.GetProperties", R"({"properties":["volume","muted","version"]})", kCallTimeoutMs, stop);
    if (!app_text.has_value()) {
        return false;
    }
    bool changed = false;
    {
        nlohmann::json app = ParseBoundedJson(*app_text);
        auto result_it = app.is_object() ? app.find("result") : app.end();
        if (result_it != app.end() && result_it->is_object()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (result_it->contains("volume") && (*result_it)["volume"].is_number_integer()) {
                state_.volume = (*result_it)["volume"].get<int>();
            }
            if (result_it->contains("muted") && (*result_it)["muted"].is_boolean()) {
                state_.muted = (*result_it)["muted"].get<bool>();
            }
            auto version_it = result_it->find("version");
            if (version_it != result_it->end() && version_it->is_object()) {
                state_.app_version = std::to_string(version_it->value("major", 0)) + "." +
                                     std::to_string(version_it->value("minor", 0));
            }
            state_.has_status = true;
            changed = true;
        }
    }

    std::optional<std::string> players_text =
        Call("Player.GetActivePlayers", "", kCallTimeoutMs, stop);
    if (!players_text.has_value()) {
        return false;
    }
    nlohmann::json players = ParseBoundedJson(*players_text);
    auto players_result = players.is_object() ? players.find("result") : players.end();
    const bool anything_playing =
        players_result != players.end() && players_result->is_array() && !players_result->empty();

    if (!anything_playing) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.now_playing.playback != KodiPlaybackState::kInactive || !state_.now_playing.title.empty()) {
                state_.now_playing = KodiNowPlaying{};
                changed = true;
            }
            state_.has_status = true;
        }
        identity_from_notification_ = false;
        if (changed) {
            event_bus_.Publish(KodiNowPlayingChangedEvent{});
        }
        return true;
    }

    int player_id = players_result->front().value("playerid", -1);
    if (player_id < 0) {
        // A -1 playerid comes back in notifications on some builds
        // (ADR-0030); GetActivePlayers itself should never return one,
        // but guard rather than send a request Kodi will reject.
        if (changed) {
            event_bus_.Publish(KodiNowPlayingChangedEvent{});
        }
        return true;
    }

    std::optional<std::string> props_text =
        Call("Player.GetProperties",
             R"({"playerid":)" + std::to_string(player_id) +
                 R"(,"properties":["speed","percentage","time","totaltime"]})",
             kCallTimeoutMs, stop);
    if (!props_text.has_value()) {
        return false;
    }
    std::optional<std::string> item_text =
        Call("Player.GetItem",
             R"({"playerid":)" + std::to_string(player_id) +
                 R"(,"properties":["title","showtitle","season","episode"]})",
             kCallTimeoutMs, stop);
    if (!item_text.has_value()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        KodiNowPlaying& np = state_.now_playing;

        nlohmann::json props = ParseBoundedJson(*props_text);
        auto props_result = props.is_object() ? props.find("result") : props.end();
        if (props_result != props.end() && props_result->is_object()) {
            np.speed = props_result->value("speed", np.speed);
            np.playback = PlaybackFromSpeed(np.speed);
            np.percent = props_result->value("percentage", np.percent);
            np.position_ms = MillisFromTimeObject(props_result->value("time", nlohmann::json::object()));
            np.duration_ms = MillisFromTimeObject(props_result->value("totaltime", nlohmann::json::object()));
        }

        // GetItem's identity is only used until a notification supplies
        // one for this playback - see identity_from_notification_.
        if (!identity_from_notification_) {
            nlohmann::json item = ParseBoundedJson(*item_text);
            auto item_result = item.is_object() ? item.find("result") : item.end();
            if (item_result != item.end() && item_result->is_object()) {
                auto item_it = item_result->find("item");
                if (item_it != item_result->end()) {
                    ApplyItemFields(*item_it, np);
                }
            }
        }
        state_.has_status = true;
        changed = true;
    }
    if (changed) {
        event_bus_.Publish(KodiNowPlayingChangedEvent{});
    }
    return true;
}

void KodiClient::HandleNotification(const std::string& frame_text) {
    nlohmann::json frame = ParseBoundedJson(frame_text);
    if (frame.is_discarded() || !frame.is_object()) {
        return;
    }
    auto method_it = frame.find("method");
    if (method_it == frame.end() || !method_it->is_string()) {
        return;
    }
    const std::string method = method_it->get<std::string>();
    nlohmann::json data = frame.value("params", nlohmann::json::object()).value("data", nlohmann::json::object());

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (method == "Application.OnVolumeChanged") {
            if (data.is_object()) {
                if (data.contains("volume") && data["volume"].is_number_integer()) {
                    state_.volume = data["volume"].get<int>();
                    changed = true;
                }
                if (data.contains("muted") && data["muted"].is_boolean()) {
                    state_.muted = data["muted"].get<bool>();
                    changed = true;
                }
            }
        } else if (method == "Player.OnStop") {
            if (state_.now_playing.playback != KodiPlaybackState::kInactive || !state_.now_playing.title.empty()) {
                state_.now_playing = KodiNowPlaying{};
                changed = true;
            }
            identity_from_notification_ = false;
        } else if (method.rfind("Player.On", 0) == 0) {
            // OnPlay / OnAVStart / OnAVChange / OnPause / OnResume /
            // OnSpeedChanged - all carry data.item (identity) and
            // data.player.speed (state). No timing fields (ADR-0030).
            if (data.is_object()) {
                auto item_it = data.find("item");
                if (item_it != data.end()) {
                    ApplyItemFields(*item_it, state_.now_playing);
                    identity_from_notification_ = true;
                }
                auto player_it = data.find("player");
                if (player_it != data.end() && player_it->is_object() && player_it->contains("speed") &&
                    (*player_it)["speed"].is_number_integer()) {
                    state_.now_playing.speed = (*player_it)["speed"].get<int>();
                }
                state_.now_playing.playback = PlaybackFromSpeed(state_.now_playing.speed);
                needs_immediate_poll_ = true;  // refresh position/duration now, not at the next interval
                changed = true;
            }
        }
        if (changed) {
            state_.has_status = true;
        }
    }
    if (changed) {
        event_bus_.Publish(KodiNowPlayingChangedEvent{});
    }
}

// --- Commands ------------------------------------------------------------

namespace {

const char* InputMethod(KodiInput input) {
    switch (input) {
        case KodiInput::kUp: return "Input.Up";
        case KodiInput::kDown: return "Input.Down";
        case KodiInput::kLeft: return "Input.Left";
        case KodiInput::kRight: return "Input.Right";
        case KodiInput::kSelect: return "Input.Select";
        case KodiInput::kBack: return "Input.Back";
        case KodiInput::kHome: return "Input.Home";
        case KodiInput::kInfo: return "Input.Info";
        case KodiInput::kContextMenu: return "Input.ContextMenu";
        case KodiInput::kShowOsd: return "Input.ShowOSD";
    }
    return "Input.Select";
}

}  // namespace

void KodiClient::EnqueueCommand(PendingCommand command) {
    command.enqueued_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        pending_commands_.push_back(std::move(command));
        while (pending_commands_.size() > kMaxPendingCommands) {
            pending_commands_.pop_front();
        }
    }
    wake_cv_.notify_one();
}

void KodiClient::PlayPause() {
    EnqueueCommand(PendingCommand{PlayerCommand{PlayerCommand::Kind::kPlayPause, 0}, "", "", false, {}});
}

void KodiClient::StopPlayback() {
    // keep_when_stale: a stop that outlasts the queue's staleness bound
    // is still worth attempting once a connection returns - it settles
    // something still happening on the box, like Harmony's `release`.
    EnqueueCommand(PendingCommand{PlayerCommand{PlayerCommand::Kind::kStop, 0}, "", "", true, {}});
}

void KodiClient::SeekPercent(double percent) {
    EnqueueCommand(PendingCommand{PlayerCommand{PlayerCommand::Kind::kSeekPercent, percent}, "", "", false, {}});
}

void KodiClient::SetSpeed(int speed) {
    EnqueueCommand(
        PendingCommand{PlayerCommand{PlayerCommand::Kind::kSetSpeed, static_cast<double>(speed)}, "", "", false, {}});
}

void KodiClient::SetVolume(int volume) {
    nlohmann::json params = {{"volume", volume}};
    EnqueueCommand(PendingCommand{std::nullopt, "Application.SetVolume", params.dump(), false, {}});
}

void KodiClient::ToggleMute() {
    // Kodi accepts the string "toggle" for the `mute` param. keep_when_stale
    // for the same reason as Stop().
    EnqueueCommand(PendingCommand{std::nullopt, "Application.SetMute", R"({"mute":"toggle"})", true, {}});
}

void KodiClient::SendInput(KodiInput input) {
    EnqueueCommand(PendingCommand{std::nullopt, InputMethod(input), "", false, {}});
}

void KodiClient::OpenLibraryItem(const std::string& id_field, long long id, bool resume) {
    nlohmann::json params = {{"item", {{id_field, id}}}};
    if (resume) {
        params["options"] = {{"resume", true}};
    }
    EnqueueCommand(PendingCommand{std::nullopt, "Player.Open", params.dump(), false, {}});
}

int KodiClient::ResolveActivePlayerId(std::stop_token stop) {
    std::optional<std::string> text = Call("Player.GetActivePlayers", "", kCallTimeoutMs, stop);
    if (!text.has_value()) {
        return -1;
    }
    nlohmann::json parsed = ParseBoundedJson(*text);
    auto result_it = parsed.is_object() ? parsed.find("result") : parsed.end();
    if (result_it == parsed.end() || !result_it->is_array() || result_it->empty()) {
        return -1;
    }
    return result_it->front().value("playerid", -1);
}

bool KodiClient::SendPendingCommands(std::stop_token stop) {
    std::deque<PendingCommand> batch;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        batch.swap(pending_commands_);
    }
    if (batch.empty()) {
        return true;
    }
    if (!ws_client_) {
        return false;  // only reached from the connected loop, but be safe
    }

    const bool needs_player_id =
        std::any_of(batch.begin(), batch.end(), [](const PendingCommand& c) { return c.player_command.has_value(); });
    const int player_id = needs_player_id ? ResolveActivePlayerId(stop) : -1;

    const auto now = std::chrono::steady_clock::now();
    bool transport_failed = false;
    for (const PendingCommand& command : batch) {
        if (stop.stop_requested()) {
            return false;
        }
        if (!command.keep_when_stale && now - command.enqueued_at > max_pending_command_age_) {
            continue;  // stale - what the user wanted then no longer reflects now
        }
        if (transport_failed && !command.keep_when_stale) {
            continue;  // socket already dead - nothing to gain from more sends
        }

        std::string method;
        std::string params_json;
        if (command.player_command.has_value()) {
            if (player_id < 0) {
                continue;  // nothing playing / resolve failed - the command has no target
            }
            const PlayerCommand& pc = *command.player_command;
            nlohmann::json params = {{"playerid", player_id}};
            switch (pc.kind) {
                case PlayerCommand::Kind::kPlayPause: method = "Player.PlayPause"; break;
                case PlayerCommand::Kind::kStop: method = "Player.Stop"; break;
                case PlayerCommand::Kind::kSeekPercent:
                    method = "Player.Seek";
                    params["value"] = {{"percentage", pc.value}};
                    break;
                case PlayerCommand::Kind::kSetSpeed:
                    method = "Player.SetSpeed";
                    params["speed"] = static_cast<int>(pc.value);
                    break;
            }
            params_json = params.dump();
        } else {
            method = command.method;
            params_json = command.params_json;
        }

        nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", ++next_rpc_id_}, {"method", method}};
        if (!params_json.empty()) {
            nlohmann::json params = ParseBoundedJson(params_json);
            if (!params.is_discarded()) {
                request["params"] = std::move(params);
            }
        }
        // Fire-and-forget: Kodi's own pushed notification (not this
        // reply) is what refreshes the snapshot; the reply frame is
        // discarded by the next PumpNotifications() (no `method` field).
        if (!ws_client_->SendText(request.dump())) {
            transport_failed = true;
        }
    }
    return !transport_failed;
}

}  // namespace homedeck
