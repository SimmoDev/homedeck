#include "core/kodi_client.h"

#include "core/host_validation.h"
#include "core/json_request.h"
#include "core/kodi_json.h"
#include "platform/websocket_message_assembler.h"
#include "third_party/nlohmann/json.hpp"

#include <algorithm>
#include <map>

namespace homedeck {

using namespace kodi_json;

namespace {

constexpr int kCallTimeoutMs = 8000;

// ReceiveText() may return a few milliseconds before the slice it was given
// has fully elapsed (millisecond truncation, scheduler granularity); a
// return this close to the busy deadline counts as having reached it.
constexpr std::chrono::milliseconds kBusySliceSlack{5};

// Library listings are fetched this many items per request so one reply
// stays far below kMaxWebSocketMessageBytes (~130 B per entry).
constexpr int kLibraryPageSize = 500;

// Bounds the merged listing against a server that reports an endless
// library; beyond this the list is truncated.
constexpr size_t kMaxLibraryItems = 10000;

// Longest wait between discovery attempts while no Kodi is found. Shorter
// than the connect backoff's cap so a Kodi that has just started is
// noticed within this long, yet long enough that an absent Kodi (the
// normal resting state on Android/Google TV) is not browsed for
// continuously.
constexpr std::chrono::milliseconds kNoTargetMaxRecheckInterval{30000};

// Bounds PumpNotifications()'s own non-blocking drain loop - the same
// role kMaxDrainIterations plays in harmony_connection.cpp. A hub/box
// that keeps a client's receive buffer permanently full can't turn one
// drain into an unbounded loop.
constexpr size_t kMaxPumpIterations = 32;
static_assert(kMaxPumpIterations >= kMaxQueuedWebSocketMessages,
              "one drain must be able to empty a full firmware receive queue");

// One mDNS browse can report the same Kodi once per interface and IP
// protocol (an IPv4 and an IPv6 answer). Keeps one entry per instance -
// keyed by its TXT `uuid`, else by name and port - preferring one with a
// resolved IPv4 address, then any resolved address.
std::vector<MdnsService> DeduplicateInstances(std::vector<MdnsService> instances) {
    const auto rank = [](const MdnsService& s) {
        if (s.address.empty()) {
            return 0;
        }
        return s.address.find(':') == std::string::npos ? 2 : 1;
    };
    std::vector<MdnsService> unique;
    std::map<std::string, size_t> index_by_key;
    for (MdnsService& s : instances) {
        const auto uuid_it = s.txt.find("uuid");
        const std::string key = (uuid_it != s.txt.end() && !uuid_it->second.empty())
                                    ? "u:" + uuid_it->second
                                    : "n:" + s.instance_name + ":" + std::to_string(s.port);
        const auto [it, inserted] = index_by_key.emplace(key, unique.size());
        if (inserted) {
            unique.push_back(std::move(s));
        } else if (rank(s) > rank(unique[it->second])) {
            unique[it->second] = std::move(s);
        }
    }
    return unique;
}

// Brackets a bare IPv6 literal so it is a valid URL authority. A
// discovered address (the only source of a ':' here - IsValidKodiHost()
// rejects one on the manual-override path) reaches both the ws:// URL
// and the resolved_host string shown on the Web UI settings page, and
// both need "host:port" to parse the way it reads.
std::string HostPortAuthority(const std::string& host, uint16_t port) {
    std::string authority = host;
    // A found ':' already implies non-empty, so authority.front() is safe.
    if (authority.find(':') != std::string::npos && authority.front() != '[') {
        authority = "[" + authority + "]";
    }
    return authority + ":" + std::to_string(port);
}

std::string WebSocketUrl(const std::string& host, uint16_t port) {
    return "ws://" + HostPortAuthority(host, port) + "/jsonrpc";
}

}  // namespace

bool IsValidKodiHost(const std::string& value) {
    // The manual-override path only: WebSocketUrl() concatenates this
    // value in without bracketing, so a bare IPv6 literal is rejected
    // (allow_colon=false), exactly as IsValidHubHost() does. A discovered
    // address bypasses this and is screened by
    // HasUnsafeHostChars(host, /*allow_colon=*/true) in ResolveTarget()
    // instead, since WebSocketUrl() brackets it. See host_validation.h.
    return !HasUnsafeHostChars(value, /*allow_colon=*/false);
}

bool IsValidKodiSetting(const std::string& key, const std::string& value) {
    return key != KodiClient::kHostKey || IsValidKodiHost(value);
}

KodiClient::KodiClient(WebSocketClientFactory make_websocket_client, MdnsBrowser& mdns_browser, Storage& storage,
                       EventBus& event_bus, std::chrono::milliseconds initial_backoff,
                       std::chrono::milliseconds max_backoff, std::chrono::milliseconds reconcile_interval,
                       std::chrono::milliseconds pump_interval, std::chrono::milliseconds browse_timeout,
                       std::chrono::milliseconds max_pending_command_age,
                       std::chrono::milliseconds library_busy_after, std::chrono::milliseconds library_call_timeout)
    : make_websocket_client_(std::move(make_websocket_client)),
      mdns_browser_(mdns_browser),
      storage_(storage),
      event_bus_(event_bus),
      backoff_(initial_backoff, max_backoff),
      no_target_backoff_(initial_backoff, std::min(max_backoff, kNoTargetMaxRecheckInterval)),
      reconcile_interval_(reconcile_interval),
      pump_interval_(pump_interval),
      browse_timeout_(browse_timeout),
      max_pending_command_age_(max_pending_command_age),
      library_busy_after_(library_busy_after),
      library_call_timeout_(library_call_timeout) {}

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

bool KodiClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.state == KodiConnectionState::kConnected;
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
        if (state_.state == state) {
            return;  // the discovery loop re-enters kDisconnected every pass
        }
        state_.state = state;
    }
    // Published after mutex_ releases - a synchronous subscriber calling
    // back into Snapshot() must not self-deadlock on the non-recursive
    // mutex_ (see HarmonyConnection::SetState()'s identical note).
    event_bus_.Publish(KodiConnectionStateChangedEvent{state});
}

void KodiClient::SetLibraryBusy(bool busy) {
    if (library_busy_ == busy) {
        return;
    }
    library_busy_ = busy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.library_busy = busy;
    }
    event_bus_.Publish(KodiNowPlayingChangedEvent{});
}

KodiClient::WakeReason KodiClient::Sleep(std::chrono::milliseconds delay, std::stop_token stop, bool watch_commands) {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    wake_cv_.wait_for(lock, stop, delay, [this, watch_commands] {
        return wake_requested_ ||
               (watch_commands && (!pending_commands_.empty() || !pending_library_requests_.empty()));
    });
    if (stop.stop_requested()) {
        return WakeReason::kStopRequested;
    }
    if (wake_requested_) {
        wake_requested_ = false;
        return WakeReason::kTriggered;
    }
    if (watch_commands && (!pending_commands_.empty() || !pending_library_requests_.empty())) {
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
            state_.target_configured = true;
            state_.selected_uuid.clear();
            state_.discovered.clear();
        }
        return Target{host_setting->value, kDefaultPort};
    }

    std::vector<MdnsService> instances = mdns_browser_.Browse(kServiceType, browse_timeout_);
    // Keep only instances we could connect to: a usable host, a non-zero
    // port, and a host string safe to concatenate into a ws://
    // authority. A discovered host never came from the user, so it
    // bypasses IsValidKodiHost() - screen it here instead, allowing ':'
    // since WebSocketUrl() brackets a resolved IPv6 literal itself.
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                   [](const MdnsService& s) {
                                       const std::string& host = !s.address.empty() ? s.address : s.hostname;
                                       return host.empty() || s.port == 0 ||
                                              HasUnsafeHostChars(host, /*allow_colon=*/true);
                                   }),
                    instances.end());
    instances = DeduplicateInstances(std::move(instances));

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

    // An instance with no resolved IP address is listed in `discovered`
    // (by its bare mDNS hostname, for manual entry) but never connected
    // to: MdnsService's header notes ".local" resolution isn't guaranteed
    // on either target, and a device can advertise a hostname with no
    // domain suffix at all ("Android", not "Android.local") that can never
    // resolve. Auto-connecting would retry an unusable target forever, so
    // `chosen` is nulled and the "ask the user to choose" state applies,
    // as for a saved-but-offline instance (ADR-0030) - no guessing.
    if (chosen != nullptr && chosen->address.empty()) {
        chosen = nullptr;
    }

    std::string resolved_host;
    std::optional<Target> target;
    if (chosen != nullptr) {
        target = Target{chosen->address, chosen->port};
        resolved_host = HostPortAuthority(chosen->address, chosen->port);
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
        state_.target_configured = !selected_uuid.empty();
        state_.selected_uuid = selected_uuid;
    }
    return target;
}

void KodiClient::ConnectionLoop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::optional<Target> target = ResolveTarget();
        if (!target.has_value()) {
            SetState(KodiConnectionState::kDisconnected);
            Sleep(no_target_backoff_.NextDelay(), stop, /*watch_commands=*/false);
            continue;
        }
        no_target_backoff_.ResetAttempts();

        SetState(KodiConnectionState::kConnecting);
        if (!ConnectAndPrime(*target, stop)) {
            if (ws_client_) {
                ws_client_->Close();
                ws_client_.reset();
            }
            SetLibraryBusy(false);
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
            if (!SendPendingLibraryRequests(stop)) {
                break;  // transport dropped mid-query
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
                if (!ReconcilePoll(stop, /*tolerate_timeout=*/true)) {
                    break;  // transport dead
                }
                last_reconcile = std::chrono::steady_clock::now();
            }
        }

        if (ws_client_) {
            ws_client_->Close();
            ws_client_.reset();
        }
        SetLibraryBusy(false);
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
    // Notifications missed while the link was down leave the previous
    // connection's identity stale, and the poll defers to notification
    // identity, so start from nothing and let the poll repopulate it.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.now_playing = KodiNowPlaying{};
    }
    identity_from_notification_ = false;
    // One reconcile poll up front so a client that connects while
    // something is already playing shows it immediately, rather than
    // blank until the first pushed notification. Doubles as a check
    // that the socket is alive.
    return ReconcilePoll(stop, /*tolerate_timeout=*/false);
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
                                            std::stop_token stop, bool report_busy) {
    if (!ws_client_) {
        return std::nullopt;
    }
    last_call_timed_out_ = false;
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

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(timeout_ms);
    const auto busy_at = started + library_busy_after_;
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            last_call_timed_out_ = ws_client_->IsOpen();
            return std::nullopt;
        }
        // Waits in two slices when the busy flag is wanted, so it can be
        // raised once library_busy_after_ has passed without a reply.
        auto wait_until = deadline;
        if (report_busy && !library_busy_ && now < busy_at) {
            wait_until = std::min(deadline, busy_at);
        }
        const int slice =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(wait_until - now).count());
        std::optional<std::string> text = ws_client_->ReceiveText(std::max(slice, 1));
        if (!text.has_value()) {
            if (wait_until < deadline && ws_client_->IsOpen() &&
                std::chrono::steady_clock::now() >= wait_until - kBusySliceSlack) {
                SetLibraryBusy(true);  // the first slice elapsed with no reply
                continue;
            }
            // A timeout (link still open) or a closed/failed transport.
            last_call_timed_out_ = ws_client_->IsOpen();
            return std::nullopt;
        }
        nlohmann::json frame = ParseBoundedJson(*text);
        if (frame.is_discarded() || !frame.is_object()) {
            continue;  // unparseable frame - ignore, keep waiting for ours
        }
        auto id_it = frame.find("id");
        if (id_it != frame.end() && id_it->is_number_integer()) {
            SetLibraryBusy(false);  // Kodi answered something, so it is not stuck on a listing
            if (id_it->get<int>() == id) {
                return text;  // our response
            }
        }
        if (frame.contains("method")) {
            HandleNotification(*text);  // an interleaved pushed notification
        }
        // else: a response to a different id (e.g. a fire-and-forget
        // command's reply) - discard and keep waiting for ours.
    }
    return std::nullopt;
}

std::optional<std::string> KodiClient::CallLibrary(const std::string& method, const std::string& params_json,
                                                    const char* result_key, std::stop_token stop, bool& truncated) {
    truncated = false;
    const int library_call_timeout_ms = static_cast<int>(library_call_timeout_.count());
    // A reply no listing parser finds a list in, so a slow listing parses to
    // an empty one while the link stays up; `truncated` tells the screen it
    // is incomplete rather than empty.
    const auto timed_out_reply = [&truncated] {
        truncated = true;
        return std::string(R"({"error":{"code":-1,"message":"timed out"}})");
    };
    // Call()'s nullopt is fatal only when the transport is dead.
    const auto survive_timeout = [&](std::optional<std::string> text) {
        return (!text.has_value() && last_call_timed_out_) ? std::optional<std::string>(timed_out_reply()) : text;
    };

    // A listing holds the loop thread for up to library_call_timeout_ per
    // request, so queued transport commands are sent before each request
    // rather than waiting out the whole listing (and aging past
    // max_pending_command_age_). This cannot help while Kodi is still
    // answering one slow request: it runs JSON-RPC calls one at a time
    // across all connections and answers nothing else until that call ends.
    if (!SendPendingCommands(stop)) {
        return std::nullopt;
    }
    if (result_key == nullptr) {
        return survive_timeout(Call(method, params_json, library_call_timeout_ms, stop, /*report_busy=*/true));
    }
    nlohmann::json params = ParseBoundedJson(params_json);
    if (!params.is_object()) {
        return std::nullopt;
    }

    nlohmann::json merged = nlohmann::json::array();
    for (long long start = 0; merged.size() < kMaxLibraryItems;) {
        if (start > 0 && !SendPendingCommands(stop)) {
            return std::nullopt;
        }
        params["limits"] = {{"start", start}, {"end", start + kLibraryPageSize}};
        std::optional<std::string> text =
            Call(method, params.dump(), library_call_timeout_ms, stop, /*report_busy=*/true);
        if (!text.has_value()) {
            if (!last_call_timed_out_) {
                return std::nullopt;  // dead transport
            }
            if (merged.empty()) {
                return timed_out_reply();
            }
            truncated = true;  // keep what arrived; the rest timed out
            break;
        }
        nlohmann::json parsed = ParseBoundedJson(*text);
        const nlohmann::json* page = ResultArray(parsed, result_key);
        if (page == nullptr) {
            if (start == 0) {
                if (parsed.is_object() && parsed.contains("error")) {
                    params.erase("limits");
                    return survive_timeout(
                        Call(method, params.dump(), library_call_timeout_ms, stop, /*report_busy=*/true));
                }
                return text;  // not a listing at all - the parser yields an empty list
            }
            truncated = true;  // a later page was an error or malformed
            break;
        }
        const size_t page_size = page->size();
        for (const nlohmann::json& item : *page) {
            if (merged.size() >= kMaxLibraryItems) {
                break;
            }
            merged.push_back(item);
        }
        start += static_cast<long long>(page_size);
        const auto result_it = parsed.find("result");
        const nlohmann::json empty = nlohmann::json::object();
        const auto limits_it = result_it->find("limits");
        const long long total = GetInt(limits_it != result_it->end() ? *limits_it : empty, "total", -1);
        // != rather than < : a server that ignored `limits` and sent
        // everything at once must not be asked for the same items again.
        if (page_size != static_cast<size_t>(kLibraryPageSize) || (total >= 0 && start >= total)) {
            break;
        }
        // Another full page exists but the cap stops the next request.
        truncated = merged.size() >= kMaxLibraryItems;
    }
    nlohmann::json result = {{"result", {{result_key, std::move(merged)}}}};
    return result.dump();
}

bool KodiClient::ReconcilePoll(std::stop_token stop, bool tolerate_timeout) {
    bool changed = false;
    // A reply that does not arrive on a link that is still open means Kodi
    // is busy (it answers one call at a time), not that the transport is
    // dead. The poll is abandoned until the next interval; what it had
    // already read stays published. kMaxToleratedPollTimeouts polls in a
    // row that get no answer at all count as a dead link: the firmware
    // client's pong timeout is 120 s and the simulator's has no keepalive.
    const auto call_failed = [&] {
        if (changed) {
            event_bus_.Publish(KodiNowPlayingChangedEvent{});
        }
        if (!tolerate_timeout || !last_call_timed_out_) {
            return false;
        }
        return ++consecutive_poll_timeouts_ < kMaxToleratedPollTimeouts;
    };
    std::optional<std::string> app_text =
        Call("Application.GetProperties", R"({"properties":["volume","muted","version"]})", kCallTimeoutMs, stop);
    if (!app_text.has_value()) {
        return call_failed();
    }
    consecutive_poll_timeouts_ = 0;  // Kodi answered, so the link is alive
    {
        nlohmann::json app = ParseBoundedJson(*app_text);
        auto result_it = app.is_object() ? app.find("result") : app.end();
        if (result_it != app.end() && result_it->is_object()) {
            std::lock_guard<std::mutex> lock(mutex_);
            // changed drives KodiNowPlayingChangedEvent, so flip it only
            // on an actual volume/mute delta - otherwise every reconcile
            // cycle would re-render the widget/screen while connected and
            // idle, when nothing about playback has moved.
            if (result_it->contains("volume") && (*result_it)["volume"].is_number_integer()) {
                int volume = (*result_it)["volume"].get<int>();
                if (volume != state_.volume) {
                    state_.volume = volume;
                    changed = true;
                }
            }
            if (result_it->contains("muted") && (*result_it)["muted"].is_boolean()) {
                bool muted = (*result_it)["muted"].get<bool>();
                if (muted != state_.muted) {
                    state_.muted = muted;
                    changed = true;
                }
            }
            auto version_it = result_it->find("version");
            if (version_it != result_it->end() && version_it->is_object()) {
                state_.app_version = std::to_string(GetInt(*version_it, "major", 0)) + "." +
                                     std::to_string(GetInt(*version_it, "minor", 0));
            }
        }
    }

    std::optional<std::string> players_text =
        Call("Player.GetActivePlayers", "", kCallTimeoutMs, stop);
    if (!players_text.has_value()) {
        return call_failed();
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
        }
        identity_from_notification_ = false;
        if (changed) {
            event_bus_.Publish(KodiNowPlayingChangedEvent{});
        }
        return true;
    }

    int player_id = static_cast<int>(GetInt(players_result->front(), "playerid", -1));
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
                 R"(,"properties":["speed","percentage","time","totaltime","canseek"]})",
             kCallTimeoutMs, stop);
    if (!props_text.has_value()) {
        return call_failed();
    }
    std::optional<std::string> item_text =
        Call("Player.GetItem",
             R"({"playerid":)" + std::to_string(player_id) +
                 R"(,"properties":["title","showtitle","season","episode"]})",
             kCallTimeoutMs, stop);
    if (!item_text.has_value()) {
        return call_failed();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        KodiNowPlaying& np = state_.now_playing;
        const KodiNowPlaying before = np;

        nlohmann::json props = ParseBoundedJson(*props_text);
        auto props_result = props.is_object() ? props.find("result") : props.end();
        if (props_result != props.end() && props_result->is_object()) {
            // GetInt/GetDouble/GetBool, not ->value(key, default) - see
            // kodi_json.h. The ->value("time"/
            // "totaltime", nlohmann::json::object()) calls below are the
            // one safe exception: their default is itself a json, so the
            // extraction is an identity conversion that can't throw
            // regardless of the field's actual type, and
            // MillisFromTimeObject() does its own per-field type checks.
            np.speed = static_cast<int>(GetInt(*props_result, "speed", np.speed));
            np.playback = PlaybackFromSpeed(np.speed);
            np.percent = GetDouble(*props_result, "percentage", np.percent);
            np.position_ms = MillisFromTimeObject(props_result->value("time", nlohmann::json::object()));
            np.duration_ms = MillisFromTimeObject(props_result->value("totaltime", nlohmann::json::object()));
            np.can_seek = GetBool(*props_result, "canseek", np.can_seek);
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
        // Only a genuine change republishes: during uninterrupted playback
        // position_ms advances every poll so this holds, but a poll that
        // lands on an unchanged paused snapshot must not re-render every
        // bound widget/screen once per reconcile interval forever - the
        // same guard the volume/mute block above and the idle path below
        // already apply.
        if (np != before) {
            changed = true;
        }
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
        // A reply nobody is waiting for any more (the call timed out): Kodi
        // answered, so it is no longer stuck on a listing.
        auto id_it = frame.find("id");
        if (id_it != frame.end() && id_it->is_number_integer()) {
            SetLibraryBusy(false);
        }
        return;
    }
    const std::string method = method_it->get<std::string>();
    // Not frame.value("params", {}).value("data", {}): .value() throws if
    // its *receiver* isn't an object, regardless of the requested type, so
    // a "params" field present but not itself an object (e.g. a string)
    // would still abort via the chained call - see GetInt()'s own comment.
    nlohmann::json data = nlohmann::json::object();
    auto params_it = frame.find("params");
    if (params_it != frame.end() && params_it->is_object()) {
        auto data_it = params_it->find("data");
        if (data_it != params_it->end() && data_it->is_object()) {
            data = *data_it;
        }
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (method == "Application.OnVolumeChanged") {
            if (data.contains("volume") && data["volume"].is_number_integer()) {
                state_.volume = data["volume"].get<int>();
                changed = true;
            }
            if (data.contains("muted") && data["muted"].is_boolean()) {
                state_.muted = data["muted"].get<bool>();
                changed = true;
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
            auto item_it = data.find("item");
            if (item_it != data.end()) {
                // OnPlay marks a new item starting. ApplyItemFields()
                // is merge-only (it never clears a field), so without
                // this a movie started straight after an episode -
                // no intervening OnStop - would keep the episode's
                // stale show_title / season / episode. Reset first,
                // then let the notification's own item repopulate.
                // OnAVChange / OnResume etc. are the *same* item and
                // must not reset.
                if (method == "Player.OnPlay") {
                    state_.now_playing.title.clear();
                    state_.now_playing.show_title.clear();
                    state_.now_playing.season = -1;
                    state_.now_playing.episode = -1;
                    state_.now_playing.media_type.clear();
                }
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

void KodiClient::SeekStep(bool forward) {
    EnqueueCommand(
        PendingCommand{PlayerCommand{PlayerCommand::Kind::kSeekStep, forward ? 1.0 : -1.0}, "", "", false, {}});
}

void KodiClient::VolumeStep(bool up) {
    EnqueueCommand(PendingCommand{std::nullopt, "Application.SetVolume",
                                  up ? R"({"volume":"increment"})" : R"({"volume":"decrement"})", false, {}});
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

void KodiClient::PlayFile(const std::string& path) {
    nlohmann::json params = {{"item", {{"file", path}}}};
    EnqueueCommand(PendingCommand{std::nullopt, "Player.Open", params.dump(), false, {}});
}

void KodiClient::EnqueueLibraryRequest(LibraryRequest request) {
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        // An identical request already waiting answers this one too. Kodi
        // runs every listing to completion, so each duplicate sent behind a
        // slow listing is more work Kodi must finish before it answers the
        // request the user is waiting for.
        if (std::find(pending_library_requests_.begin(), pending_library_requests_.end(), request) !=
            pending_library_requests_.end()) {
            return;
        }
        pending_library_requests_.push_back(std::move(request));
        while (pending_library_requests_.size() > kMaxPendingLibraryRequests) {
            pending_library_requests_.pop_front();
        }
    }
    wake_cv_.notify_one();
}

void KodiClient::RequestMovies() { EnqueueLibraryRequest({LibraryRequest::Kind::kMovies}); }

void KodiClient::RequestTvShows() { EnqueueLibraryRequest({LibraryRequest::Kind::kTvShows}); }

void KodiClient::RequestSeasons(long long tvshowid) {
    EnqueueLibraryRequest({LibraryRequest::Kind::kSeasons, tvshowid});
}

void KodiClient::RequestEpisodes(long long tvshowid, int season) {
    EnqueueLibraryRequest({LibraryRequest::Kind::kEpisodes, tvshowid, season});
}

void KodiClient::RequestArtists() { EnqueueLibraryRequest({LibraryRequest::Kind::kArtists}); }

void KodiClient::RequestAlbums(long long artistid) {
    EnqueueLibraryRequest({LibraryRequest::Kind::kAlbums, artistid});
}

void KodiClient::RequestSongs(long long albumid) { EnqueueLibraryRequest({LibraryRequest::Kind::kSongs, albumid}); }

void KodiClient::RequestFileSources() { EnqueueLibraryRequest({LibraryRequest::Kind::kFileSources}); }

void KodiClient::RequestDirectory(const std::string& path) {
    EnqueueLibraryRequest({LibraryRequest::Kind::kDirectory, 0, 0, path});
}

void KodiClient::RequestChannelGroups() { EnqueueLibraryRequest({LibraryRequest::Kind::kChannelGroups}); }

void KodiClient::RequestChannels(long long channelgroupid) {
    EnqueueLibraryRequest({LibraryRequest::Kind::kChannels, channelgroupid});
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
    return static_cast<int>(GetInt(result_it->front(), "playerid", -1));
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
                case PlayerCommand::Kind::kSeekStep:
                    method = "Player.Seek";
                    params["value"] = pc.value > 0 ? "smallforward" : "smallbackward";
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

namespace {

std::string Params(const nlohmann::json& params) { return params.dump(); }

}  // namespace

KodiClient::LibraryQuery KodiClient::BuildLibraryQuery(const LibraryRequest& request) {
    // Sorted by label - Kodi's own database order (insertion order) has
    // no relation to how a user browses.
    const auto sort_by = [](const char* method) {
        return nlohmann::json{{"method", method}, {"order", "ascending"}};
    };
    const nlohmann::json kSortByLabel = sort_by("label");

    switch (request.kind) {
        case LibraryRequest::Kind::kMovies:
            return {"VideoLibrary.GetMovies",
                    Params({{"properties", {"title", "year", "resume"}}, {"sort", kSortByLabel}}),
                    "movies",
                    [this](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiMoviesFetchedEvent{ParseMovies(text), truncated});
                    }};
        case LibraryRequest::Kind::kTvShows:
            return {"VideoLibrary.GetTVShows",
                    Params({{"properties", {"title", "year", "episode", "watchedepisodes"}}, {"sort", kSortByLabel}}),
                    "tvshows",
                    [this](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiTvShowsFetchedEvent{ParseTvShows(text), truncated});
                    }};
        case LibraryRequest::Kind::kSeasons:
            return {"VideoLibrary.GetSeasons",
                    Params({{"tvshowid", request.parent_id},
                     {"properties", {"season", "episode", "watchedepisodes"}},
                     {"sort", sort_by("season")}}),
                    "seasons",
                    [this, tvshowid = request.parent_id](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiSeasonsFetchedEvent{tvshowid, ParseSeasons(text), truncated});
                    }};
        case LibraryRequest::Kind::kEpisodes:
            return {"VideoLibrary.GetEpisodes",
                    Params({{"tvshowid", request.parent_id},
                     {"season", request.season},
                     {"properties", {"episode", "title", "resume"}},
                     {"sort", sort_by("episode")}}),
                    "episodes",
                    [this, tvshowid = request.parent_id, season = request.season](const std::string& text,
                                                                                    bool truncated) {
                        event_bus_.Publish(KodiEpisodesFetchedEvent{tvshowid, season, ParseEpisodes(text), truncated});
                    }};
        case LibraryRequest::Kind::kArtists:
            // artistid/artist/label are returned without naming properties.
            return {"AudioLibrary.GetArtists",
                    Params({{"sort", kSortByLabel}}),
                    "artists",
                    [this](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiArtistsFetchedEvent{ParseArtists(text), truncated});
                    }};
        case LibraryRequest::Kind::kAlbums:
            return {"AudioLibrary.GetAlbums",
                    Params({{"filter", {{"artistid", request.parent_id}}},
                     {"properties", {"title", "year"}},
                     {"sort", sort_by("year")}}),
                    "albums",
                    [this, artistid = request.parent_id](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiAlbumsFetchedEvent{artistid, ParseAlbums(text), truncated});
                    }};
        case LibraryRequest::Kind::kSongs:
            return {"AudioLibrary.GetSongs",
                    Params({{"filter", {{"albumid", request.parent_id}}},
                     {"properties", {"title", "track", "duration"}},
                     {"sort", sort_by("track")}}),
                    "songs",
                    [this, albumid = request.parent_id](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiSongsFetchedEvent{albumid, ParseSongs(text), truncated});
                    }};
        case LibraryRequest::Kind::kFileSources:
            return {"Files.GetSources",
                    Params({{"media", "video"}}),
                    nullptr,
                    [this](const std::string& text, bool truncated) {
                        event_bus_.Publish(
                            KodiFilesFetchedEvent{"", ParseFileItems(text, "sources", /*all_folders=*/true), truncated});
                    }};
        case LibraryRequest::Kind::kDirectory:
            return {"Files.GetDirectory",
                    Params({{"directory", request.path}, {"media", "video"}}),
                    "files",
                    [this, path = request.path](const std::string& text, bool truncated) {
                        event_bus_.Publish(
                            KodiFilesFetchedEvent{path, ParseFileItems(text, "files", /*all_folders=*/false), truncated});
                    }};
        case LibraryRequest::Kind::kChannelGroups:
            return {"PVR.GetChannelGroups",
                    Params({{"channeltype", "tv"}}),
                    nullptr,
                    [this](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiChannelGroupsFetchedEvent{ParseChannelGroups(text), truncated});
                    }};
        case LibraryRequest::Kind::kChannels:
            return {"PVR.GetChannels",
                    Params({{"channelgroupid", request.parent_id}}),
                    "channels",
                    [this, channelgroupid = request.parent_id](const std::string& text, bool truncated) {
                        event_bus_.Publish(KodiChannelsFetchedEvent{channelgroupid, ParseChannels(text), truncated});
                    }};
    }
    return {};
}

bool KodiClient::SendPendingLibraryRequests(std::stop_token stop) {
    std::deque<LibraryRequest> batch;
    {
        std::lock_guard<std::mutex> lock(wake_mutex_);
        batch.swap(pending_library_requests_);
    }
    if (batch.empty()) {
        return true;
    }
    if (!ws_client_) {
        return false;  // only reached from the connected loop, but be safe
    }

    for (const LibraryRequest& request : batch) {
        if (stop.stop_requested()) {
            return false;
        }
        const LibraryQuery query = BuildLibraryQuery(request);
        bool truncated = false;
        std::optional<std::string> text =
            CallLibrary(query.method, query.params_json, query.result_key, stop, truncated);
        if (!text.has_value()) {
            return false;
        }
        query.publish(*text, truncated);
    }
    return true;
}

}  // namespace homedeck
