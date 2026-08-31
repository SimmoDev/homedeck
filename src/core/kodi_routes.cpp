#include "core/kodi_routes.h"

#include "third_party/nlohmann/json.hpp"

namespace homedeck {

namespace {

const char* StateToString(KodiConnectionState state) {
    switch (state) {
        case KodiConnectionState::kDisconnected: return "disconnected";
        case KodiConnectionState::kConnecting: return "connecting";
        case KodiConnectionState::kConnected: return "connected";
        case KodiConnectionState::kError: return "error";
    }
    return "disconnected";
}

const char* PlaybackToString(KodiPlaybackState state) {
    switch (state) {
        case KodiPlaybackState::kInactive: return "inactive";
        case KodiPlaybackState::kPlaying: return "playing";
        case KodiPlaybackState::kPaused: return "paused";
    }
    return "inactive";
}

nlohmann::json SnapshotToJson(const KodiSnapshot& s) {
    nlohmann::json instances = nlohmann::json::array();
    for (const KodiDiscoveredInstance& i : s.discovered) {
        instances.push_back({{"name", i.name}, {"host", i.host}, {"uuid", i.uuid}});
    }
    const KodiNowPlaying& np = s.now_playing;
    return {
        {"state", StateToString(s.state)},
        {"resolvedHost", s.resolved_host},
        {"discovered", std::move(instances)},
        {"appVersion", s.app_version},
        {"volume", s.volume},
        {"muted", s.muted},
        {"nowPlaying",
         {
             {"playback", PlaybackToString(np.playback)},
             {"speed", np.speed},
             {"title", np.title},
             {"showTitle", np.show_title},
             {"season", np.season},
             {"episode", np.episode},
             {"mediaType", np.media_type},
             {"positionMs", np.position_ms},
             {"durationMs", np.duration_ms},
             {"percent", np.percent},
             {"canSeek", np.can_seek},
         }},
    };
}

}  // namespace

void RegisterKodiRoutes(HttpServer& server, KodiClient& kodi_client, AdminAuthService& auth) {
    server.RegisterHandler(HttpMethod::kGet, "/api/kodi/status",
                           auth.RequireAuth([&kodi_client](const HttpRequest&) {
                               return HttpResponse{200, "application/json",
                                                   SnapshotToJson(kodi_client.Snapshot()).dump(), {}};
                           }));

    server.RegisterHandler(HttpMethod::kPost, "/api/kodi/reconnect",
                           auth.RequireAuth([&kodi_client](const HttpRequest&) {
                               kodi_client.TriggerReconnect();
                               return HttpResponse{200, "application/json", R"({"status":"ok"})", {}};
                           }));
}

}  // namespace homedeck
