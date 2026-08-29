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

// Rejects the same shapes IsValidHubHost() does (scheme prefix, embedded
// whitespace/control bytes, a path, `#`/`?`/`@`, a bare IPv6 literal) and
// for the same reasons - WebSocketUrl() below concatenates this value
// straight into a URL. Empty is accepted: an empty `host` override means
// "use discovery instead," not a malformed address (see
// ConnectionLoop()). Mirrors webui/src/lib/kodiValidation.ts.
bool IsValidKodiHost(const std::string& value);

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
    KodiConnectionState state = KodiConnectionState::kDisconnected;
    // True once any poll or notification has populated the fields below
    // this session - lets the UI tell "connected, nothing playing" from
    // "not connected yet."
    bool has_status = false;
    // The host:port the connection loop last resolved a target to (from
    // the `host` override, or the chosen discovered instance) - shown on
    // the Web UI settings page. Empty when no target could be resolved.
    std::string resolved_host;
    // Instances seen by the most recent discovery browse. size() > 1 with
    // no saved selection is the "ask the user to choose" case (ADR-0030).
    // Empty while a manual `host` override is in effect (no browse runs).
    std::vector<KodiDiscoveredInstance> discovered;
    std::string app_version;
    int volume = 0;
    bool muted = false;
    KodiNowPlaying now_playing;
};

struct KodiConnectionStateChangedEvent {
    KodiConnectionState state;
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
// Scope (M4a): discover/select a Kodi instance, connect its JSON-RPC
// WebSocket, and keep a fresh snapshot of connection state, app
// volume/mute, and what's playing - driven by Kodi's own pushed
// notifications, with a periodic reconcile-poll as the liveness check
// and a backstop for any missed push. Playback/input commands and
// library browsing are separate, later pieces (roadmap M4).
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
               std::chrono::milliseconds max_pending_command_age = std::chrono::seconds(5));

    // Module:
    void Start() override;
    void Stop() override;

    KodiSnapshot Snapshot() const;

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
    // drops entries older than max_pending_command_age (StopPlayback() and mute
    // are exempt - like Harmony's `release`, they stop/settle something
    // already happening and stay worth attempting once a connection
    // exists, however late).
    void PlayPause();
    void StopPlayback();
    void SeekPercent(double percent);   // 0..100
    void SetSpeed(int speed);           // Kodi's speed ladder: -32..-2, 1, 2..32
    void SetVolume(int volume);         // 0..100
    void ToggleMute();
    void SendInput(KodiInput input);
    // Starts playback of a library item, e.g. OpenLibraryItem("movieid",
    // 42) or ("episodeid", 958). `resume` picks up from the stored
    // resume point. Player.Open (ADR-0030) - the browse screens (M4b)
    // are the caller.
    void OpenLibraryItem(const std::string& id_field, long long id, bool resume);

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
        enum class Kind { kPlayPause, kStop, kSeekPercent, kSetSpeed };
        Kind kind;
        double value = 0;  // percent for kSeekPercent, speed for kSetSpeed
    };
    struct PendingCommand {
        std::optional<PlayerCommand> player_command;
        std::string method;       // set iff player_command is empty
        std::string params_json;  // "" for a no-param method
        bool keep_when_stale = false;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    // Bounds how many commands accumulate while disconnected/reconnecting
    // (a bug tap-storm, or the UI queuing while offline) - the oldest is
    // dropped first once full, same policy and reasoning as
    // HarmonyConnection::kMaxPendingCommands.
    static constexpr size_t kMaxPendingCommands = 20;

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
    std::optional<std::string> Call(const std::string& method, const std::string& params_json, int timeout_ms,
                                    std::stop_token stop);
    // Application.GetProperties + Player.GetActivePlayers (+ GetProperties
    // /GetItem when something is playing). Refreshes the snapshot and is
    // the periodic liveness probe. false => transport dead, reconnect.
    bool ReconcilePoll(std::stop_token stop);
    // Applies one pushed notification (Player.On* / Application.OnVolumeChanged),
    // given as raw frame text, to the snapshot and publishes
    // KodiNowPlayingChangedEvent if anything changed. Sets
    // needs_immediate_poll_ on a play-state transition so the loop
    // refreshes position/duration right away rather than at the next
    // reconcile interval (notifications carry no timing fields - ADR-0030).
    void HandleNotification(const std::string& frame_text);
    void SetState(KodiConnectionState state);
    // watch_commands: only the connected inner loop watches
    // pending_commands_ (ws_client_ exists only then) - the
    // no-target/backoff waits leave commands queued rather than
    // busy-waking on them, same as HarmonyConnection::Sleep().
    WakeReason Sleep(std::chrono::milliseconds delay, std::stop_token stop, bool watch_commands);

    void EnqueueCommand(PendingCommand command);
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
    std::chrono::milliseconds reconcile_interval_;
    std::chrono::milliseconds pump_interval_;
    std::chrono::milliseconds browse_timeout_;
    std::chrono::milliseconds max_pending_command_age_;

    // Owned by, and only ever touched from, task_'s own thread - no
    // mutex, same single-owner reasoning as HarmonyConnection::ws_client_.
    std::unique_ptr<WebSocketClient> ws_client_;
    int next_rpc_id_ = 0;
    bool needs_immediate_poll_ = false;
    // Once a Player.On* notification has supplied identity for the
    // current playback, the reconcile poll's Player.GetItem result is
    // no longer used to overwrite it - the notification's `item` is the
    // authoritative identity source, and for add-on playback GetItem
    // returns blanks (ADR-0030). Reset when playback stops. GetItem is
    // still polled and used as the *initial* identity when a client
    // connects to a Kodi that is already playing (no notification seen).
    bool identity_from_notification_ = false;

    mutable std::mutex mutex_;
    KodiSnapshot state_;

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    bool wake_requested_ = false;
    // Guarded by wake_mutex_ too (one wake channel for both a trigger
    // and a queued command - Sleep() tells them apart), same as
    // HarmonyConnection::pending_commands_.
    std::deque<PendingCommand> pending_commands_;

    std::unique_ptr<Task> task_;
};

}  // namespace homedeck
