#pragma once

#include "core/event_bus.h"
#include "core/module.h"
#include "core/retry_backoff.h"
#include "core/storage.h"
#include "platform/mdns_browser.h"
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
#include <utility>

namespace homedeck {

// See ADR-0030 for the protocol this is built against: Kodi's JSON-RPC
// API over an unauthenticated WebSocket on port 9090, path /jsonrpc,
// with server-pushed notifications for player and volume state.
enum class KodiConnectionState {
    kDisconnected,  // no instance selected/discovered, or not yet attempted
    kConnecting,
    kConnected,
    kError,  // most recent attempt failed; a retry is scheduled. Does NOT
             // publish a NotificationEvent - on Android/Google TV, Kodi
             // being down is a normal resting state, not a fault worth
             // alerting on (ADR-0030).
};

enum class KodiPlaybackState { kInactive, kPlaying, kPaused };

// The Kodi UI-navigation actions the Touch UI's remote screen sends -
// each maps to a dedicated Input.* JSON-RPC method (ADR-0030 prefers
// those over Input.ExecuteAction, whose action-name validation was
// inconsistent on the reference build).
enum class KodiInput { kUp, kDown, kLeft, kRight, kSelect, kBack, kHome, kInfo, kContextMenu, kShowOsd };

// The manual `host` override's server-side check: thin wrapper over
// HasUnsafeHostChars(value, /*allow_colon=*/false) (see
// core/host_validation.h), same as Harmony's IsValidHubHost(). Empty is
// accepted - an empty override means "use discovery instead," not a
// malformed address (see ConnectionLoop()). Mirrors
// webui/src/lib/kodiValidation.ts.
bool IsValidKodiHost(const std::string& value);

// Validates one of KodiClient's settings for POST /api/settings (ui/app_core.cpp
// registers it by module id): `host` through IsValidKodiHost(), every other
// key unconstrained.
bool IsValidKodiSetting(const std::string& key, const std::string& value);

// What's playing right now, merged from two sources per ADR-0030:
// identity (title/show/episode) comes from the Player.On* notification's
// own `item`, which is populated even for add-on playback where
// Player.GetItem returns blanks; timing comes from Player.GetProperties.
struct KodiNowPlaying {
    KodiPlaybackState playback = KodiPlaybackState::kInactive;
    // Kodi's raw player speed: 0 paused, 1 playing, >1 / <0 the
    // fast-forward / rewind ladder. Retained so the UI can show a
    // "x2" / "rewind" affordance rather than only play/pause.
    int speed = 0;
    std::string title;
    std::string show_title;  // episodes only; empty otherwise
    int season = -1;
    int episode = -1;
    std::string media_type;  // "episode" / "movie" / "song" / "unknown"
    long long position_ms = 0;
    long long duration_ms = 0;
    double percent = 0.0;
    // From Player.GetProperties' own "canseek" - false for some live/add-on
    // sources Kodi can play but not scrub through. Polled only (no
    // equivalent field on any Player.On* notification), so it lags behind
    // position/duration by up to one reconcile cycle; resets to false with
    // the rest of this struct once playback stops.
    bool can_seek = false;

    // Lets ReconcilePoll() tell a poll that actually moved something from
    // one that found the exact same state (a long pause), so it only
    // republishes KodiNowPlayingChangedEvent on an actual delta.
    bool operator==(const KodiNowPlaying&) const = default;
};

// One Kodi instance seen by the most recent discovery browse - enough
// for the Web UI settings page to render a "pick one" list keyed by the
// stable `uuid` (ADR-0030).
struct KodiDiscoveredInstance {
    std::string name;
    std::string host;
    std::string uuid;
};

struct KodiSnapshot {
    // state == kConnected is only ever published after ConnectAndPrime()'s
    // initial ReconcilePoll() has populated the fields below, so it doubles
    // as "a snapshot is available" - no separate flag needed.
    KodiConnectionState state = KodiConnectionState::kDisconnected;
    // The host:port the connection loop last resolved a target to (from
    // the `host` override, or the chosen discovered instance) - shown on
    // the Web UI settings page. Empty when no target could be resolved.
    std::string resolved_host;
    // A manual `host` override or a saved `instance_uuid` exists, whether
    // or not that target is currently reachable. Distinguishes "configured
    // but Kodi is not running" from "nothing configured" when state is
    // kDisconnected.
    bool target_configured = false;
    // The saved `instance_uuid`, empty when none is saved or a manual
    // `host` override is in effect.
    std::string selected_uuid;
    // Instances seen by the most recent discovery browse. size() > 1 with
    // no saved selection is the "ask the user to choose" case (ADR-0030).
    // Empty while a manual `host` override is in effect (no browse runs).
    std::vector<KodiDiscoveredInstance> discovered;
    std::string app_version;
    int volume = 0;
    bool muted = false;
    KodiNowPlaying now_playing;
    // A library request has waited longer than the busy threshold for its
    // reply. Kodi runs JSON-RPC calls one at a time across all connections,
    // so until it answers, playback and navigation commands cannot get
    // through either. Clears when Kodi next replies to anything, and on
    // disconnect.
    bool library_busy = false;
};

// One library entry per M4b's video-browsing screens (movies / TV shows /
// seasons / episodes). Deliberately thin - just enough for a list button
// and a Play/Resume choice; artwork stays out of scope (ADR-0030).
struct KodiMovie {
    long long movieid = -1;
    std::string title;
    int year = 0;
    // > 0 => Kodi has a stored resume point (VideoLibrary's own "resume"
    // property, {position, total} in seconds) - the browse screen offers
    // Resume as well as Play when this is set.
    long long resume_position_ms = 0;
};

struct KodiTvShow {
    long long tvshowid = -1;
    std::string title;
    int year = 0;
    int episode_count = 0;
    int watched_episode_count = 0;
};

struct KodiSeason {
    // Kodi's own season number, not a database seasonid - what
    // RequestEpisodes() filters by (0 == Specials).
    int season = 0;
    std::string label;  // e.g. "Season 1" - Kodi's own formatted label
    int episode_count = 0;
    int watched_episode_count = 0;
};

struct KodiEpisode {
    long long episodeid = -1;
    int episode = 0;
    std::string title;
    long long resume_position_ms = 0;
};

// Music library browsing (also M4b): Artists -> Albums -> Songs, no
// fourth Play/Resume detail level unlike the video screens above -
// AudioLibrary.GetSongs has no "resume" property at all (requesting one
// is rejected with "Invalid params"), so a song plays directly on tap.
struct KodiArtist {
    long long artistid = -1;
    std::string name;
};

struct KodiAlbum {
    long long albumid = -1;
    std::string title;
    int year = 0;
};

struct KodiSong {
    long long songid = -1;
    int track = 0;
    std::string title;
    int duration_seconds = 0;
};

// Raw filesystem browsing (also M4b) - the fallback view for content not
// (yet) scraped into the video library, mirroring Kodi's own Videos >
// Files menu: Files.GetSources("video") for the configured source list,
// then Files.GetDirectory(path, "video") at arbitrary depth (a show's
// season folders, a season's episode files, ...). Unlike the fixed-depth
// screens above, folder depth is unbounded, so KodiFilesScreen keeps its
// own path stack rather than a fixed set of sibling containers. No
// Play/Resume choice, same reasoning as music - a file plays directly on
// tap via PlayFile(), Player.Open with a raw {"file": path} item instead
// of a library id.
struct KodiFileItem {
    // Kodi's own "file" field - an opaque path string, re-passed
    // verbatim to Files.GetDirectory (to descend) or PlayFile() (to
    // play); never parsed or displayed itself, only `label` is shown.
    std::string path;
    std::string label;
    bool is_folder = false;
};

// Live TV browsing (also M4b): channel groups -> channels, two levels -
// shallower than every other browse type since there's nothing to
// scope a channel by beyond its group. Scoped to channeltype "tv" only
// (radio is a separate PVR channel type Kodi also supports but this
// pass doesn't browse). No Play/Resume choice - a live broadcast has no
// resume point - so a channel plays directly on tap via the existing
// OpenLibraryItem("channelid", id, /*resume=*/false), the same
// generic-id_field mechanism the video screens use for movieid/
// episodeid.
struct KodiChannelGroup {
    long long channelgroupid = -1;
    std::string label;
};

struct KodiChannel {
    long long channelid = -1;
    std::string label;
};

struct KodiConnectionStateChangedEvent {
    KodiConnectionState state;
};

// Published once RequestMovies()/RequestTvShows()/RequestSeasons()/
// RequestEpisodes()'s query completes - see each request method's own
// comment. Unlike KodiNowPlayingChangedEvent these carry the data itself
// (there is no standing "library snapshot" to re-read via Snapshot() -
// the library is too large to hold in full, and a browse screen only
// ever wants the one list it just asked for).
//
// Every event with a `truncated` flag reports an incomplete listing: it
// reached kMaxLibraryItems with more available, a later page failed, or a
// call timed out. A timeout before anything arrived yields an empty list
// with `truncated` set, so a screen can tell a slow listing from an empty one.
struct KodiMoviesFetchedEvent {
    std::vector<KodiMovie> movies;
    bool truncated = false;
};
struct KodiTvShowsFetchedEvent {
    std::vector<KodiTvShow> shows;
    bool truncated = false;
};
struct KodiSeasonsFetchedEvent {
    long long tvshowid;
    std::vector<KodiSeason> seasons;
    bool truncated = false;
};
struct KodiEpisodesFetchedEvent {
    long long tvshowid;
    int season;
    std::vector<KodiEpisode> episodes;
    bool truncated = false;
};
struct KodiArtistsFetchedEvent {
    std::vector<KodiArtist> artists;
    bool truncated = false;
};
struct KodiAlbumsFetchedEvent {
    long long artistid;
    std::vector<KodiAlbum> albums;
    bool truncated = false;
};
struct KodiSongsFetchedEvent {
    long long albumid;
    std::vector<KodiSong> songs;
    bool truncated = false;
};
// path is "" for the top-level sources list (RequestFileSources()), or
// the directory just listed (RequestDirectory()) - lets a screen ignore
// a reply for a directory it's since navigated away from, same
// filtering KodiSeasonsFetchedEvent/KodiEpisodesFetchedEvent use.
struct KodiFilesFetchedEvent {
    std::string path;
    std::vector<KodiFileItem> items;
    bool truncated = false;
};
struct KodiChannelGroupsFetchedEvent {
    std::vector<KodiChannelGroup> groups;
    bool truncated = false;
};
struct KodiChannelsFetchedEvent {
    long long channelgroupid;
    std::vector<KodiChannel> channels;
    bool truncated = false;
};

// Marker only - handlers call Snapshot(), same shape as
// HarmonyConfigUpdatedEvent. Published on any change to volume/mute or
// to what's playing (including transport position on the reconcile
// cycle), so a Now Playing widget/screen can re-render from one
// subscription.
struct KodiNowPlayingChangedEvent {};

// The Kodi module's connection manager - the second Module
// implementation after HarmonyConnection (ADR-0003's contract test).
// Portable (HttpClient is not needed at all - pure WebSocket; built on
// MdnsBrowser&, a WebSocketClient factory, Storage&, EventBus&), and
// host-testable via fakes for all four.
//
// Scope: discover/select a Kodi instance, connect its JSON-RPC
// WebSocket, and keep a fresh snapshot of connection state, app
// volume/mute, and what's playing - driven by Kodi's own pushed
// notifications, with a periodic reconcile-poll as the liveness check
// and a backstop for any missed push. It also queues playback/input
// commands and request/response library browse queries onto the same
// connection loop.
//
// Unlike HarmonyConnection this class correlates JSON-RPC responses to
// requests by numeric `id` while applying interleaved notifications,
// since Kodi pushes unsolicited frames on the same socket at any time
// (see Call()).
class KodiClient : public Module {
public:
    using WebSocketClientFactory = std::function<std::unique_ptr<WebSocketClient>()>;

    // module_id "kodi" - see ADR-0012's per-module Storage namespacing.
    static constexpr char kModuleId[] = "kodi";
    // Manual address override; empty/unset means "use discovery".
    static constexpr char kHostKey[] = "host";
    // The chosen discovered instance, stored as its mDNS TXT `uuid`
    // rather than an IP (ADR-0030) - survives DHCP changes.
    static constexpr char kInstanceUuidKey[] = "instance_uuid";

    static constexpr char kServiceType[] = "_xbmc-jsonrpc._tcp";
    static constexpr uint16_t kDefaultPort = 9090;

    // browse_timeout / reconcile_interval / pump_interval /
    // max_pending_command_age are injectable (defaulted to production
    // values) so tests don't wait out the production discovery/poll
    // cadences - the same "production default, test-overridable" shape
    // HarmonyConnection uses.
    KodiClient(WebSocketClientFactory make_websocket_client, MdnsBrowser& mdns_browser, Storage& storage,
               EventBus& event_bus, std::chrono::milliseconds initial_backoff = std::chrono::seconds(2),
               std::chrono::milliseconds max_backoff = std::chrono::seconds(60),
               std::chrono::milliseconds reconcile_interval = std::chrono::seconds(10),
               std::chrono::milliseconds pump_interval = std::chrono::milliseconds(250),
               std::chrono::milliseconds browse_timeout = std::chrono::seconds(2),
               std::chrono::milliseconds max_pending_command_age = std::chrono::seconds(5),
               std::chrono::milliseconds library_busy_after = std::chrono::seconds(3),
               std::chrono::milliseconds library_call_timeout = std::chrono::seconds(30));

    // Module:
    void Start() override;
    void Stop() override;

    KodiSnapshot Snapshot() const;
    // Cheaper than Snapshot() when only the connection state matters.
    bool IsConnected() const;

    // Wakes the connection loop immediately so a newly-saved host/
    // instance selection is tried without waiting out the current
    // backoff or reconcile delay - the Web UI settings save flow calls
    // this. Same shape as HarmonyConnection::TriggerReconnect().
    void TriggerReconnect();

    // Playback / navigation commands. All safe to call from any thread
    // (e.g. a Touch UI screen on the UI thread): they queue the intent
    // onto the connection loop's own thread, which owns ws_client_.
    // Fire-and-forget - Kodi's own pushed notifications, not a reply to
    // these, are what refresh the snapshot. A no-op if never connected;
    // the queue is bounded and drops the oldest entry when full, and
    // drops entries older than max_pending_command_age. StopPlayback()
    // and mute are exempt from that staleness drop - like Harmony's
    // `release`, they settle something already happening on the box, so
    // an aged-out one is still sent on the next drain rather than
    // discarded. None of this survives a transport failure mid-drain:
    // the in-flight batch is not requeued (SendPendingCommands()). Kodi's
    // push-driven UI re-syncs on the next reconcile, so nothing is left
    // stuck by a lost command.
    void PlayPause();
    void StopPlayback();
    // Coarse relative seek / volume nudge - Kodi's own "smallforward"/
    // "smallbackward" and "increment"/"decrement" step verbs, so rapid
    // repeated taps stack server-side instead of every one computing the
    // same target from a snapshot that only refreshes on a Kodi push.
    // NowPlayingScreen's transport row uses these.
    void SeekStep(bool forward);
    void VolumeStep(bool up);
    void SeekPercent(double percent);   // 0..100, absolute
    void SetSpeed(int speed);           // Kodi's speed ladder: -32..-2, 1, 2..32
    void SetVolume(int volume);         // 0..100, absolute
    void ToggleMute();
    void SendInput(KodiInput input);
    // Starts playback of a library item, e.g. OpenLibraryItem("movieid",
    // 42) or ("episodeid", 958). `resume` picks up from the stored
    // resume point. Player.Open (ADR-0030) - the browse screens (M4b)
    // are the caller.
    void OpenLibraryItem(const std::string& id_field, long long id, bool resume);
    // Starts playback of a raw filesystem item by its Kodi "file" path
    // string (KodiFileItem::path) rather than a library id - the
    // Files-browse screen's own Player.Open shape, no resume choice (see
    // KodiFileItem's own comment).
    void PlayFile(const std::string& path);

    // Library browse queries (M4b). Unlike the commands above these are
    // request/response, not fire-and-forget: each queues onto the
    // connection loop, which issues the matching VideoLibrary.Get*/
    // AudioLibrary.Get* call via Call() (not SendText - a reply is the
    // entire point) and publishes the corresponding KodiXFetchedEvent
    // with the parsed result. Safe to call from any thread; queues until
    // a connection exists rather than failing outright (same
    // bounded/drop-oldest shape as pending_commands_, see
    // kMaxPendingLibraryRequests) - a screen calls these once, on
    // becoming visible, not on a timer, so there is no separate retry if
    // it's still queued when the screen navigates away.
    void RequestMovies();
    void RequestTvShows();
    void RequestSeasons(long long tvshowid);
    void RequestEpisodes(long long tvshowid, int season);
    void RequestArtists();
    void RequestAlbums(long long artistid);
    void RequestSongs(long long albumid);
    // "" lists Files.GetSources("video") (the top-level source list);
    // any other value lists Files.GetDirectory(path, "video") - both
    // publish KodiFilesFetchedEvent.
    void RequestFileSources();
    void RequestDirectory(const std::string& path);
    void RequestChannelGroups();
    void RequestChannels(long long channelgroupid);

private:
    struct Target {
        std::string host;
        uint16_t port = kDefaultPort;
    };

    enum class WakeReason { kTimeout, kTriggered, kCommandPending, kStopRequested };

    // A player command whose params can't be built until the connection
    // loop's own thread resolves the active playerid (see
    // SendPendingCommands()). A global command (volume/mute/input/open)
    // needs no playerid, so its method/params are pre-built at enqueue
    // and player_command stays empty.
    struct PlayerCommand {
        enum class Kind { kPlayPause, kStop, kSeekPercent, kSeekStep, kSetSpeed };
        Kind kind;
        double value = 0;  // percent for kSeekPercent, speed for kSetSpeed,
                           // sign only (>0 forward) for kSeekStep
    };
    struct PendingCommand {
        std::optional<PlayerCommand> player_command;
        std::string method;       // set iff player_command is empty
        std::string params_json;  // "" for a no-param method
        bool keep_when_stale = false;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    // Bounds how many commands accumulate while disconnected/reconnecting
    // (a tap storm, or the UI queuing while offline) - the oldest is
    // dropped first once full, same policy and reasoning as
    // HarmonyConnection::kMaxPendingCommands.
    static constexpr size_t kMaxPendingCommands = 20;

    // One queued library browse query (RequestMovies() etc.) - unlike
    // PendingCommand these are drained via Call() (request/response),
    // not SendText(), since the whole point is the reply. tvshowid/season
    // are unused for kMovies/kTvShows.
    struct LibraryRequest {
        enum class Kind { kMovies, kTvShows, kSeasons, kEpisodes, kArtists, kAlbums, kSongs, kFileSources,
                          kDirectory, kChannelGroups, kChannels };
        // A constructor rather than an aggregate: ESP-IDF's warning set
        // flags a partial aggregate initialisation.
        LibraryRequest(Kind kind, long long parent_id = 0, int season = 0, std::string path = "")
            : kind(kind), parent_id(parent_id), season(season), path(std::move(path)) {}

        Kind kind;
        // The parent id a query is scoped to - tvshowid for
        // kSeasons/kEpisodes, artistid for kAlbums, albumid for kSongs,
        // channelgroupid for kChannels; unused for kMovies/kTvShows/
        // kArtists/kFileSources/kDirectory/kChannelGroups (nothing to
        // scope by, or scoped by `path` instead).
        long long parent_id;
        int season;          // kEpisodes only - the second id it needs alongside parent_id
        std::string path;    // kDirectory only - the Files.GetDirectory path to list
    };
    // What one LibraryRequest asks Kodi and how its reply is published.
    // result_key is null for a listing that is not paged (see CallLibrary()).
    struct LibraryQuery {
        const char* method = "";
        std::string params_json;
        const char* result_key = nullptr;
        std::function<void(const std::string& text, bool truncated)> publish;
    };

    // Lower than kMaxPendingCommands - a browse screen issues at most one
    // request per user action (a tap into a show/season), so a deep
    // backlog only happens while disconnected, and nothing needs more
    // than a handful of the most recent taps preserved for when a
    // connection returns.
    static constexpr size_t kMaxPendingLibraryRequests = 8;

    void ConnectionLoop(std::stop_token stop);
    // Reads the `host`/`instance_uuid` settings and, if discovery is in
    // play, runs one MdnsBrowser::Browse(). Returns the address to
    // connect to, or nullopt when none can be resolved (no override, and
    // either nothing discovered or more than one instance with no saved
    // selection - the "ask the user to choose" case). Updates
    // discovered / resolved_host on the snapshot as a side effect.
    std::optional<Target> ResolveTarget();
    // Opens the WebSocket and runs one initial reconcile poll. true only
    // if both succeeded, leaving ws_client_ open and owned by the
    // calling thread.
    bool ConnectAndPrime(const Target& target, std::stop_token stop);
    // Non-blocking drain of every already-buffered frame, dispatching
    // notifications to HandleNotification() - called each pump cycle.
    void PumpNotifications();
    // Sends one JSON-RPC request (params_json is a raw JSON object
    // string, or "" for none) and returns the raw text of the response
    // frame whose `id` matches, processing any interleaved notification
    // frames on the way. nullopt on send failure, timeout, or a closed/
    // errored socket (the caller treats that as the connection being
    // dead - it doubles as the liveness check). Only the connection
    // loop's own thread calls this: it is the sole owner of ws_client_
    // and next_rpc_id_. nlohmann::json is kept out of this header, same
    // as HarmonyConnection - the .cpp does all parsing.
    // report_busy: set KodiSnapshot::library_busy once the reply has taken
    // longer than library_busy_after_ (for library listings).
    std::optional<std::string> Call(const std::string& method, const std::string& params_json, int timeout_ms,
                                    std::stop_token stop, bool report_busy = false);
    // A library listing via Call(), with the longer library_call_timeout_.
    // With a non-null result_key the listing is fetched in kLibraryPageSize
    // pages (Kodi's `limits` parameter) and merged into one
    // {"result":{<result_key>:[...]}} text, so no single reply approaches
    // kMaxWebSocketMessageBytes however large the library is, and a slow
    // source can't hold one reply past the timeout. Capped at
    // kMaxLibraryItems items. A Kodi that rejects `limits` with a JSON-RPC
    // error gets one unpaged retry. A call that times out on a connection
    // that is still open (a slow share) does not tear the link down: the
    // pages merged so far are returned as a truncated listing, or an error
    // reply when there are none, and the late reply is ignored by id.
    // `truncated` is set when the listing is
    // incomplete (see KodiMoviesFetchedEvent::truncated). Queued playback
    // commands are sent before each request (SendPendingCommands()), so a
    // listing of many pages does not hold them back; Kodi itself answers
    // nothing else while one request is slow. nullopt on the same conditions
    // as Call(), or when sending those commands fails.
    std::optional<std::string> CallLibrary(const std::string& method, const std::string& params_json,
                                           const char* result_key, std::stop_token stop, bool& truncated);
    // Application.GetProperties + Player.GetActivePlayers (+ GetProperties
    // /GetItem when something is playing). Refreshes the snapshot and is
    // the periodic liveness probe. false => transport dead, reconnect.
    // tolerate_timeout: a call that times out on a still-open link ends the
    // poll with true instead (Kodi is busy, not gone). The up-front poll in
    // ConnectAndPrime() does not tolerate it: it is what proves a new
    // connection answers.
    bool ReconcilePoll(std::stop_token stop, bool tolerate_timeout);
    // Applies one pushed notification (Player.On* / Application.OnVolumeChanged),
    // given as raw frame text, to the snapshot and publishes
    // KodiNowPlayingChangedEvent if anything changed. Sets
    // needs_immediate_poll_ on a play-state transition so the loop
    // refreshes position/duration right away rather than at the next
    // reconcile interval (notifications carry no timing fields - ADR-0030).
    void HandleNotification(const std::string& frame_text);
    void SetState(KodiConnectionState state);
    // Updates the snapshot's library_busy and, on a change, publishes
    // KodiNowPlayingChangedEvent so the control screens re-render.
    void SetLibraryBusy(bool busy);
    // watch_commands: only the connected inner loop watches
    // pending_commands_/pending_library_requests_ (ws_client_ exists only
    // then) - the no-target/backoff waits leave both queued rather than
    // busy-waking on them, same as HarmonyConnection::Sleep().
    WakeReason Sleep(std::chrono::milliseconds delay, std::stop_token stop, bool watch_commands);

    void EnqueueCommand(PendingCommand command);
    void EnqueueLibraryRequest(LibraryRequest request);
    LibraryQuery BuildLibraryQuery(const LibraryRequest& request);
    // Drains pending_library_requests_ and, for each, issues the matching
    // VideoLibrary.Get* Call() and publishes its KodiXFetchedEvent.
    // Unlike SendPendingCommands() a send failure or a closed transport is
    // fatal to the whole batch (false => reconnect) rather than something
    // later entries can route around - a query is worthless without its
    // reply, so there is no "keep_when_stale"-style partial-success case to
    // preserve. A timeout on an open connection is not fatal (see
    // CallLibrary()). Loop-thread only, same ws_client_ ownership as Call().
    bool SendPendingLibraryRequests(std::stop_token stop);
    // Drains pending_commands_ and sends each - fire-and-forget SendText,
    // not Call(): a command's reply carries no state this class needs
    // (Kodi pushes the resulting state change separately), and the pump
    // discards the reply frame. Resolves the active playerid once, up
    // front, only if any queued entry needs one. Drops stale entries
    // (keep_when_stale exempt) and stops sending non-exempt entries once
    // a send has failed (transport gone). Returns false on a send
    // failure so the caller reconnects rather than looping on a dead
    // socket. Loop-thread only.
    bool SendPendingCommands(std::stop_token stop);
    // Sends Player.GetActivePlayers and returns the first video/audio
    // player's id, or -1 if nothing is playing / the call failed.
    int ResolveActivePlayerId(std::stop_token stop);

    WebSocketClientFactory make_websocket_client_;
    MdnsBrowser& mdns_browser_;
    Storage& storage_;
    EventBus& event_bus_;

    RetryBackoff backoff_;
    // Spaces out discovery attempts while no target resolves.
    RetryBackoff no_target_backoff_;
    std::chrono::milliseconds reconcile_interval_;
    std::chrono::milliseconds pump_interval_;
    std::chrono::milliseconds browse_timeout_;
    std::chrono::milliseconds max_pending_command_age_;
    std::chrono::milliseconds library_busy_after_;
    // A directory on a cold network share can take well past the 8 s control
    // call timeout to list. A listing that times out on an open connection
    // keeps the link (see CallLibrary()); Call() itself reports any timeout
    // as nullopt.
    std::chrono::milliseconds library_call_timeout_;

    // Owned by, and only ever touched from, task_'s own thread - no
    // mutex, same single-owner reasoning as HarmonyConnection::ws_client_.
    std::unique_ptr<WebSocketClient> ws_client_;
    int next_rpc_id_ = 0;
    // Set by Call() when it returned nullopt because no reply arrived in
    // time on a connection that is still open, as opposed to a dead
    // transport. Only meaningful right after a Call() that returned nullopt.
    bool last_call_timed_out_ = false;
    bool needs_immediate_poll_ = false;
    // Loop-thread mirror of KodiSnapshot::library_busy, so Call() can check
    // it for every reply without taking mutex_.
    bool library_busy_ = false;
    // Once a Player.On* notification has supplied identity for the
    // current playback, the reconcile poll's Player.GetItem result does
    // not overwrite it - the notification's `item` is the
    // authoritative identity source, and for add-on playback GetItem
    // returns blanks (ADR-0030). Reset when playback stops. GetItem is
    // still polled and used as the *initial* identity when a client
    // connects to a Kodi that is already playing (no notification seen).
    bool identity_from_notification_ = false;

    mutable std::mutex mutex_;
    KodiSnapshot state_;

    std::mutex wake_mutex_;
    // _any for the stop_token-aware wait_for(): a stop request cannot be
    // missed between the predicate check and the block.
    std::condition_variable_any wake_cv_;
    bool wake_requested_ = false;
    // Guarded by wake_mutex_ too (one wake channel for both a trigger
    // and a queued command - Sleep() tells them apart), same as
    // HarmonyConnection::pending_commands_.
    std::deque<PendingCommand> pending_commands_;
    // Same wake channel/mutex as pending_commands_ above - Sleep()
    // watches both, and the connected loop drains both every wake.
    std::deque<LibraryRequest> pending_library_requests_;

    std::unique_ptr<Task> task_;
};

}  // namespace homedeck
