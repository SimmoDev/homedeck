#include "core/kodi_client.h"

#include "core/host_validation.h"
#include "core/json_request.h"
#include "third_party/nlohmann/json.hpp"

#include <algorithm>

namespace homedeck {

namespace {

constexpr int kCallTimeoutMs = 8000;

// A directory on a cold network share can take well past kCallTimeoutMs to
// list, and a timeout is treated as a dead transport (see Call()).
constexpr int kLibraryCallTimeoutMs = 30000;

// Library listings are fetched this many items per request so one reply
// stays far below kMaxWebSocketMessageBytes (~130 B per entry).
constexpr int kLibraryPageSize = 500;

// Bounds the merged listing against a server that reports an endless
// library; beyond this the list is truncated.
constexpr size_t kMaxLibraryItems = 10000;
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

// nlohmann::json::value()/get<T>() throw json::type_error when a present
// field (or the receiver itself) is not the requested type - and firmware
// builds with C++ exceptions disabled (CONFIG_COMPILER_CXX_EXCEPTIONS is
// unset), so json.hpp's JSON_THROW resolves to std::abort() there instead
// of throw. Kodi's 9090 API has no authentication (ADR-0030), so any
// device on the LAN can trigger this with a malformed reply. These three
// helpers check both the receiver's and the field's type before
// extracting, the same hazard weather_routes.cpp's geocode parsing
// already guards against - never reach for j.value(key, default) on data
// from an external response.
long long GetInt(const nlohmann::json& j, const char* key, long long fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : fallback;
}

double GetDouble(const nlohmann::json& j, const char* key, double fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : fallback;
}

std::string GetString(const nlohmann::json& j, const char* key, const std::string& fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

bool GetBool(const nlohmann::json& j, const char* key, bool fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
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

long long MillisFromTimeObject(const nlohmann::json& t) {
    long long seconds = GetInt(t, "hours", 0) * 3600LL + GetInt(t, "minutes", 0) * 60LL + GetInt(t, "seconds", 0);
    return seconds * 1000LL + GetInt(t, "milliseconds", 0);
}

KodiPlaybackState PlaybackFromSpeed(int speed) {
    return speed == 0 ? KodiPlaybackState::kPaused : KodiPlaybackState::kPlaying;
}

// A movie/episode's "resume" property - {"position": <seconds, float>,
// "total": <seconds, float>} - not the {hours,minutes,seconds,
// milliseconds} shape MillisFromTimeObject() parses. 0 (no resume point)
// if absent/malformed.
long long ResumePositionMs(const nlohmann::json& item) {
    if (!item.is_object()) {
        return 0;
    }
    auto resume_it = item.find("resume");
    if (resume_it == item.end()) {
        return 0;
    }
    return static_cast<long long>(GetDouble(*resume_it, "position", 0.0) * 1000.0);
}

// title, falling back to Kodi's own always-present `label` when the
// requested `title` property comes back blank - same fallback
// ApplyItemFields() uses for Now Playing's add-on-playback case.
std::string TitleOrLabel(const nlohmann::json& item) {
    std::string title = GetString(item, "title", "");
    return !title.empty() ? title : GetString(item, "label", "");
}

// Pulls the named array out of a VideoLibrary.Get*'s "result" object
// (e.g. "movies", "tvshows", "seasons", "episodes") - an empty array,
// not a parse error, for anything short of a well-formed object
// containing it (a malformed/absent field just means an empty list).
const nlohmann::json* ResultArray(const nlohmann::json& parsed, const char* key) {
    auto result_it = parsed.is_object() ? parsed.find("result") : parsed.end();
    if (result_it == parsed.end() || !result_it->is_object()) {
        return nullptr;
    }
    auto array_it = result_it->find(key);
    if (array_it == result_it->end() || !array_it->is_array()) {
        return nullptr;
    }
    return &*array_it;
}

std::vector<KodiMovie> ParseMovies(const std::string& text) {
    std::vector<KodiMovie> movies;
    // Named, not inline in the ResultArray() call - ResultArray() returns
    // a pointer into whatever nlohmann::json it's given, so the parsed
    // value has to outlive that pointer's use below, not end at the end
    // of the call expression the way an inline temporary would.
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "movies");
    if (array == nullptr) {
        return movies;
    }
    movies.reserve(array->size());
    for (const auto& m : *array) {
        KodiMovie movie;
        movie.movieid = GetInt(m, "movieid", -1);
        movie.title = TitleOrLabel(m);
        movie.year = static_cast<int>(GetInt(m, "year", 0));
        movie.resume_position_ms = ResumePositionMs(m);
        movies.push_back(std::move(movie));
    }
    return movies;
}

std::vector<KodiTvShow> ParseTvShows(const std::string& text) {
    std::vector<KodiTvShow> shows;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "tvshows");
    if (array == nullptr) {
        return shows;
    }
    shows.reserve(array->size());
    for (const auto& s : *array) {
        KodiTvShow show;
        show.tvshowid = GetInt(s, "tvshowid", -1);
        show.title = TitleOrLabel(s);
        show.year = static_cast<int>(GetInt(s, "year", 0));
        show.episode_count = static_cast<int>(GetInt(s, "episode", 0));
        show.watched_episode_count = static_cast<int>(GetInt(s, "watchedepisodes", 0));
        shows.push_back(std::move(show));
    }
    return shows;
}

std::vector<KodiSeason> ParseSeasons(const std::string& text) {
    std::vector<KodiSeason> seasons;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "seasons");
    if (array == nullptr) {
        return seasons;
    }
    seasons.reserve(array->size());
    for (const auto& s : *array) {
        KodiSeason season;
        season.season = static_cast<int>(GetInt(s, "season", 0));
        season.label = TitleOrLabel(s);
        season.episode_count = static_cast<int>(GetInt(s, "episode", 0));
        season.watched_episode_count = static_cast<int>(GetInt(s, "watchedepisodes", 0));
        seasons.push_back(std::move(season));
    }
    return seasons;
}

std::vector<KodiEpisode> ParseEpisodes(const std::string& text) {
    std::vector<KodiEpisode> episodes;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "episodes");
    if (array == nullptr) {
        return episodes;
    }
    episodes.reserve(array->size());
    for (const auto& e : *array) {
        KodiEpisode episode;
        episode.episodeid = GetInt(e, "episodeid", -1);
        episode.episode = static_cast<int>(GetInt(e, "episode", 0));
        episode.title = TitleOrLabel(e);
        episode.resume_position_ms = ResumePositionMs(e);
        episodes.push_back(std::move(episode));
    }
    return episodes;
}

std::vector<KodiArtist> ParseArtists(const std::string& text) {
    std::vector<KodiArtist> artists;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "artists");
    if (array == nullptr) {
        return artists;
    }
    artists.reserve(array->size());
    for (const auto& a : *array) {
        KodiArtist artist;
        artist.artistid = GetInt(a, "artistid", -1);
        // "artist" (Kodi's own artist-name field), falling back to the
        // always-present `label` - same fallback shape as TitleOrLabel(),
        // just a different primary field name (artists have no "title").
        std::string name = GetString(a, "artist", "");
        artist.name = !name.empty() ? name : GetString(a, "label", "");
        artists.push_back(std::move(artist));
    }
    return artists;
}

std::vector<KodiAlbum> ParseAlbums(const std::string& text) {
    std::vector<KodiAlbum> albums;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "albums");
    if (array == nullptr) {
        return albums;
    }
    albums.reserve(array->size());
    for (const auto& a : *array) {
        KodiAlbum album;
        album.albumid = GetInt(a, "albumid", -1);
        album.title = TitleOrLabel(a);
        album.year = static_cast<int>(GetInt(a, "year", 0));
        albums.push_back(std::move(album));
    }
    return albums;
}

std::vector<KodiSong> ParseSongs(const std::string& text) {
    std::vector<KodiSong> songs;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "songs");
    if (array == nullptr) {
        return songs;
    }
    songs.reserve(array->size());
    for (const auto& s : *array) {
        KodiSong song;
        song.songid = GetInt(s, "songid", -1);
        song.track = static_cast<int>(GetInt(s, "track", 0));
        song.title = TitleOrLabel(s);
        song.duration_seconds = static_cast<int>(GetInt(s, "duration", 0));
        songs.push_back(std::move(song));
    }
    return songs;
}

// Shared by Files.GetSources' reply ("sources") and Files.GetDirectory's
// ("files") - result_key and all_folders are the only differences
// between the two shapes. A source item carries no "filetype" field at
// all - it's always a folder by definition (a configured root), so all_folders=true skips
// the "filetype" check entirely rather than reading a field that isn't
// there and misreading every source as a file.
std::vector<KodiFileItem> ParseFileItems(const std::string& text, const char* result_key, bool all_folders) {
    std::vector<KodiFileItem> items;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, result_key);
    if (array == nullptr) {
        return items;
    }
    items.reserve(array->size());
    for (const auto& f : *array) {
        KodiFileItem item;
        item.path = GetString(f, "file", "");
        item.label = TitleOrLabel(f);
        item.is_folder = all_folders || GetString(f, "filetype", "") == "directory";
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<KodiChannelGroup> ParseChannelGroups(const std::string& text) {
    std::vector<KodiChannelGroup> groups;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "channelgroups");
    if (array == nullptr) {
        return groups;
    }
    groups.reserve(array->size());
    for (const auto& g : *array) {
        KodiChannelGroup group;
        group.channelgroupid = GetInt(g, "channelgroupid", -1);
        group.label = TitleOrLabel(g);
        groups.push_back(std::move(group));
    }
    return groups;
}

std::vector<KodiChannel> ParseChannels(const std::string& text) {
    std::vector<KodiChannel> channels;
    nlohmann::json parsed = ParseBoundedJson(text);
    const nlohmann::json* array = ResultArray(parsed, "channels");
    if (array == nullptr) {
        return channels;
    }
    channels.reserve(array->size());
    for (const auto& c : *array) {
        KodiChannel channel;
        channel.channelid = GetInt(c, "channelid", -1);
        channel.label = TitleOrLabel(c);
        channels.push_back(std::move(channel));
    }
    return channels;
}

// Pulls title/show/season/episode/type out of a notification's or a
// Player.GetItem response's `item` object. Missing/blank fields are left
// at their struct defaults so a later, better-populated source (or the
// notification, for add-on playback - see ADR-0030) can fill them.
void ApplyItemFields(const nlohmann::json& item, KodiNowPlaying& now_playing) {
    if (!item.is_object()) {
        return;
    }
    std::string title = GetString(item, "title", "");
    if (title.empty()) {
        title = GetString(item, "label", "");  // add-on playback: `title` blank, `label` usable
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
    // The manual-override path only: WebSocketUrl() concatenates this
    // value in without bracketing, so a bare IPv6 literal is rejected
    // (allow_colon=false), exactly as IsValidHubHost() does. A discovered
    // address bypasses this and is screened by
    // HasUnsafeHostChars(host, /*allow_colon=*/true) in ResolveTarget()
    // instead, since WebSocketUrl() brackets it. See host_validation.h.
    return !HasUnsafeHostChars(value, /*allow_colon=*/false);
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
        return wake_requested_ ||
               (watch_commands && (!pending_commands_.empty() || !pending_library_requests_.empty())) ||
               stop.stop_requested();
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
    // Keep only instances we could actually connect to: a usable host,
    // a non-zero port, and a host string safe to concatenate into a
    // ws:// authority. A discovered host never came from the user, so it
    // bypasses IsValidKodiHost() - screen it here instead, allowing ':'
    // since WebSocketUrl() brackets a resolved IPv6 literal itself.
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                                   [](const MdnsService& s) {
                                       const std::string& host = !s.address.empty() ? s.address : s.hostname;
                                       return host.empty() || s.port == 0 ||
                                              HasUnsafeHostChars(host, /*allow_colon=*/true);
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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!stop.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            last_call_timed_out_ = ws_client_->IsOpen();
            return std::nullopt;
        }
        const int remaining =
            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        std::optional<std::string> text = ws_client_->ReceiveText(std::max(remaining, 1));
        if (!text.has_value()) {
            // A timeout (link still open) or a closed/failed transport.
            last_call_timed_out_ = ws_client_->IsOpen();
            return std::nullopt;
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

std::optional<std::string> KodiClient::CallLibrary(const std::string& method, const std::string& params_json,
                                                    const char* result_key, std::stop_token stop, bool& truncated) {
    truncated = false;
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

    // A listing holds the loop thread for up to kLibraryCallTimeoutMs per
    // request, so queued transport commands are sent before each request
    // rather than waiting out the whole listing (and aging past
    // max_pending_command_age_).
    if (!SendPendingCommands(stop)) {
        return std::nullopt;
    }
    if (result_key == nullptr) {
        return survive_timeout(Call(method, params_json, kLibraryCallTimeoutMs, stop));
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
        std::optional<std::string> text = Call(method, params.dump(), kLibraryCallTimeoutMs, stop);
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
                    return survive_timeout(Call(method, params.dump(), kLibraryCallTimeoutMs, stop));
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
        const KodiNowPlaying before = np;

        nlohmann::json props = ParseBoundedJson(*props_text);
        auto props_result = props.is_object() ? props.find("result") : props.end();
        if (props_result != props.end() && props_result->is_object()) {
            // GetInt/GetDouble/GetBool, not ->value(key, default) - see
            // this file's own top-of-file comment. The ->value("time"/
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
        pending_library_requests_.push_back(request);
        while (pending_library_requests_.size() > kMaxPendingLibraryRequests) {
            pending_library_requests_.pop_front();
        }
    }
    wake_cv_.notify_one();
}

// Every field named explicitly below, even when a Kind leaves it at its
// struct default - a partial aggregate-init (e.g. just {kind, parent_id})
// still triggers -Wmissing-field-initializers under ESP-IDF's stricter
// default warning set (not under this project's host/simulator build
// flags, which is how this went unnoticed) for any field left out,
// designated or not.
void KodiClient::RequestMovies() {
    EnqueueLibraryRequest(LibraryRequest{.kind = LibraryRequest::Kind::kMovies, .parent_id = 0, .season = 0, .path = ""});
}

void KodiClient::RequestTvShows() {
    EnqueueLibraryRequest(LibraryRequest{.kind = LibraryRequest::Kind::kTvShows, .parent_id = 0, .season = 0, .path = ""});
}

void KodiClient::RequestSeasons(long long tvshowid) {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kSeasons, .parent_id = tvshowid, .season = 0, .path = ""});
}

void KodiClient::RequestEpisodes(long long tvshowid, int season) {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kEpisodes, .parent_id = tvshowid, .season = season, .path = ""});
}

void KodiClient::RequestArtists() {
    EnqueueLibraryRequest(LibraryRequest{.kind = LibraryRequest::Kind::kArtists, .parent_id = 0, .season = 0, .path = ""});
}

void KodiClient::RequestAlbums(long long artistid) {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kAlbums, .parent_id = artistid, .season = 0, .path = ""});
}

void KodiClient::RequestSongs(long long albumid) {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kSongs, .parent_id = albumid, .season = 0, .path = ""});
}

void KodiClient::RequestFileSources() {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kFileSources, .parent_id = 0, .season = 0, .path = ""});
}

void KodiClient::RequestDirectory(const std::string& path) {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kDirectory, .parent_id = 0, .season = 0, .path = path});
}

void KodiClient::RequestChannelGroups() {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kChannelGroups, .parent_id = 0, .season = 0, .path = ""});
}

void KodiClient::RequestChannels(long long channelgroupid) {
    EnqueueLibraryRequest(
        LibraryRequest{.kind = LibraryRequest::Kind::kChannels, .parent_id = channelgroupid, .season = 0, .path = ""});
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

    // Sorted by label - Kodi's own database order (insertion order) has
    // no relation to how a user browses, unlike the movie/show grouping
    // Devices/ActivitiesScreen preserve from Harmony's own hub order.
    static const nlohmann::json kSortByLabel = {{"method", "label"}, {"order", "ascending"}};

    for (const LibraryRequest& request : batch) {
        if (stop.stop_requested()) {
            return false;
        }
        bool truncated = false;
        switch (request.kind) {
            case LibraryRequest::Kind::kMovies: {
                nlohmann::json params = {{"properties", {"title", "year", "resume"}}, {"sort", kSortByLabel}};
                std::optional<std::string> text =
                    CallLibrary("VideoLibrary.GetMovies", params.dump(), "movies", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiMoviesFetchedEvent{ParseMovies(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kTvShows: {
                nlohmann::json params = {{"properties", {"title", "year", "episode", "watchedepisodes"}},
                                         {"sort", kSortByLabel}};
                std::optional<std::string> text =
                    CallLibrary("VideoLibrary.GetTVShows", params.dump(), "tvshows", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiTvShowsFetchedEvent{ParseTvShows(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kSeasons: {
                nlohmann::json params = {{"tvshowid", request.parent_id},
                                         {"properties", {"season", "episode", "watchedepisodes"}},
                                         {"sort", {{"method", "season"}, {"order", "ascending"}}}};
                std::optional<std::string> text =
                    CallLibrary("VideoLibrary.GetSeasons", params.dump(), "seasons", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiSeasonsFetchedEvent{request.parent_id, ParseSeasons(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kEpisodes: {
                nlohmann::json params = {{"tvshowid", request.parent_id},
                                         {"season", request.season},
                                         {"properties", {"episode", "title", "resume"}},
                                         {"sort", {{"method", "episode"}, {"order", "ascending"}}}};
                std::optional<std::string> text =
                    CallLibrary("VideoLibrary.GetEpisodes", params.dump(), "episodes", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(
                    KodiEpisodesFetchedEvent{request.parent_id, request.season, ParseEpisodes(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kArtists: {
                // No "properties" needed - artistid/artist/label are all
                // returned by default, the same as movieid/label elsewhere.
                nlohmann::json params = {{"sort", kSortByLabel}};
                std::optional<std::string> text =
                    CallLibrary("AudioLibrary.GetArtists", params.dump(), "artists", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiArtistsFetchedEvent{ParseArtists(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kAlbums: {
                nlohmann::json params = {{"filter", {{"artistid", request.parent_id}}},
                                         {"properties", {"title", "year"}},
                                         {"sort", {{"method", "year"}, {"order", "ascending"}}}};
                std::optional<std::string> text =
                    CallLibrary("AudioLibrary.GetAlbums", params.dump(), "albums", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiAlbumsFetchedEvent{request.parent_id, ParseAlbums(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kSongs: {
                nlohmann::json params = {{"filter", {{"albumid", request.parent_id}}},
                                         {"properties", {"title", "track", "duration"}},
                                         {"sort", {{"method", "track"}, {"order", "ascending"}}}};
                std::optional<std::string> text =
                    CallLibrary("AudioLibrary.GetSongs", params.dump(), "songs", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiSongsFetchedEvent{request.parent_id, ParseSongs(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kFileSources: {
                nlohmann::json params = {{"media", "video"}};
                std::optional<std::string> text =
                    CallLibrary("Files.GetSources", params.dump(), nullptr, stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiFilesFetchedEvent{"", ParseFileItems(*text, "sources", /*all_folders=*/true), truncated});
                break;
            }
            case LibraryRequest::Kind::kDirectory: {
                nlohmann::json params = {{"directory", request.path}, {"media", "video"}};
                std::optional<std::string> text =
                    CallLibrary("Files.GetDirectory", params.dump(), "files", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(
                    KodiFilesFetchedEvent{request.path, ParseFileItems(*text, "files", /*all_folders=*/false), truncated});
                break;
            }
            case LibraryRequest::Kind::kChannelGroups: {
                nlohmann::json params = {{"channeltype", "tv"}};
                std::optional<std::string> text =
                    CallLibrary("PVR.GetChannelGroups", params.dump(), nullptr, stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiChannelGroupsFetchedEvent{ParseChannelGroups(*text), truncated});
                break;
            }
            case LibraryRequest::Kind::kChannels: {
                nlohmann::json params = {{"channelgroupid", request.parent_id}};
                std::optional<std::string> text =
                    CallLibrary("PVR.GetChannels", params.dump(), "channels", stop, truncated);
                if (!text.has_value()) {
                    return false;
                }
                event_bus_.Publish(KodiChannelsFetchedEvent{request.parent_id, ParseChannels(*text), truncated});
                break;
            }
        }
    }
    return true;
}

}  // namespace homedeck
