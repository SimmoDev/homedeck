#include "debug_kodi_backend.h"

#include "core/kodi_client.h"
#include "platform/host/websocket_client.h"
#include "third_party/nlohmann/json.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace homedeck::sim {

namespace {

// Canned reconcile-poll answers: a video player 37% through a 45-minute
// episode, seekable. Static - enough to prove the connected-state layout
// renders; motion isn't the point of a manual screenshot check. An empty
// return means "no reply for this method" - the fire-and-forget
// transport/nav commands, whose replies KodiClient never reads.
std::string CannedResult(const std::string& method) {
    if (method == "Application.GetProperties") {
        return R"({"volume":45,"muted":false,"version":{"major":21,"minor":2}})";
    }
    if (method == "Player.GetActivePlayers") {
        return R"([{"playerid":1,"type":"video"}])";
    }
    if (method == "Player.GetProperties") {
        return R"({"speed":1,"percentage":37.0,)"
               R"("time":{"hours":0,"minutes":16,"seconds":39,"milliseconds":0},)"
               R"("totaltime":{"hours":0,"minutes":45,"seconds":0,"milliseconds":0},"canseek":true})";
    }
    if (method == "Player.GetItem") {
        return R"({"item":{"title":"The One With The Simulator","showtitle":"Simulated Show",)"
               R"("season":3,"episode":7,"type":"episode"}})";
    }
    // M4b library-browse screens (KodiMoviesScreen/KodiTvShowsScreen) -
    // enough canned data to check layout/reachability for each level,
    // same "not a Kodi stand-in" scope as the player state above.
    if (method == "VideoLibrary.GetMovies") {
        return R"({"movies":[)"
               R"({"movieid":1,"title":"Simulated Movie One","year":2021,"resume":{"position":0,"total":0}},)"
               R"({"movieid":2,"title":"Simulated Movie Two","year":2019,)"
               R"("resume":{"position":1830.0,"total":5400.0}}]})";
    }
    if (method == "VideoLibrary.GetTVShows") {
        return R"({"tvshows":[{"tvshowid":1,"title":"Simulated Show","year":2020,)"
               R"("episode":16,"watchedepisodes":7}]})";
    }
    if (method == "VideoLibrary.GetSeasons") {
        return R"({"seasons":[)"
               R"({"season":1,"label":"Season 1","episode":8,"watchedepisodes":8},)"
               R"({"season":2,"label":"Season 2","episode":8,"watchedepisodes":0}]})";
    }
    if (method == "VideoLibrary.GetEpisodes") {
        return R"({"episodes":[)"
               R"({"episodeid":1,"episode":1,"title":"Simulated Pilot","resume":{"position":0,"total":0}},)"
               R"({"episodeid":2,"episode":2,"title":"Simulated Episode Two",)"
               R"("resume":{"position":600.0,"total":1320.0}}]})";
    }
    if (method == "AudioLibrary.GetArtists") {
        return R"({"artists":[{"artistid":1,"artist":"Simulated Artist","label":"Simulated Artist"}]})";
    }
    if (method == "AudioLibrary.GetAlbums") {
        return R"({"albums":[{"albumid":1,"title":"Simulated Album","label":"Simulated Album","year":2022}]})";
    }
    if (method == "AudioLibrary.GetSongs") {
        return R"({"songs":[)"
               R"({"songid":1,"track":1,"title":"Simulated Track One","label":"Simulated Track One","duration":210},)"
               R"({"songid":2,"track":2,"title":"Simulated Track Two","label":"Simulated Track Two","duration":185}]})";
    }
    if (method == "Files.GetSources") {
        return R"({"sources":[{"file":"/simulated/media/","label":"Simulated Media"}]})";
    }
    if (method == "Files.GetDirectory") {
        return R"({"files":[)"
               R"({"file":"/simulated/media/Simulated Folder/","filetype":"directory","label":"Simulated Folder"},)"
               R"({"file":"/simulated/media/Simulated File.mkv","filetype":"file","label":"Simulated File"}]})";
    }
    if (method == "PVR.GetChannelGroups") {
        return R"({"channelgroups":[{"channelgroupid":1,"channeltype":"tv","label":"Simulated Channels"}]})";
    }
    if (method == "PVR.GetChannels") {
        return R"({"channels":[{"channelid":1,"channeltype":"tv","label":"Simulated Channel One"},)"
               R"({"channelid":2,"channeltype":"tv","label":"Simulated Channel Two"}]})";
    }
    return "";
}

// The WebSocketClient KodiClient's factory hands back: a fake for a Kodi
// JSON-RPC connect while armed (see Connect()), otherwise a delegating
// HostWebSocketClient (Harmony's hub, a Kodi on the LAN).
class DebugKodiWebSocketClient : public WebSocketClient {
public:
    explicit DebugKodiWebSocketClient(const std::atomic<bool>& armed) : armed_(armed) {}

    bool Connect(const std::string& url) override {
        // Armed, every Kodi JSON-RPC WebSocket is the fake - whether the
        // target came from the fake mDNS instance or a stale manual host
        // override (settings/kodi/host), which ResolveTarget() prefers
        // over discovery. Harmony's socket (a different path) and, while
        // disarmed, everything fall through to HostWebSocketClient.
        fake_ = armed_.load() && url.find("/jsonrpc") != std::string::npos;
        if (!fake_) {
            real_ = std::make_unique<HostWebSocketClient>();
            return real_->Connect(url);
        }
        open_ = true;
        return true;
    }

    bool SendText(const std::string& text) override {
        if (!fake_) {
            return real_ && real_->SendText(text);
        }
        if (!open_ || !armed_.load()) {
            return false;
        }
        nlohmann::json request = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (request.is_object() && request.contains("id") && request.contains("method")) {
            std::string result = CannedResult(request["method"].get<std::string>());
            if (!result.empty()) {
                nlohmann::json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
                reply["result"] = nlohmann::json::parse(result, nullptr, /*allow_exceptions=*/false);
                std::lock_guard<std::mutex> lock(mutex_);
                replies_.push_back(reply.dump());
            }
        }
        return true;
    }

    std::optional<std::string> ReceiveText(int timeout_ms) override {
        if (!fake_) {
            return real_ ? real_->ReceiveText(timeout_ms) : std::nullopt;
        }
        // Disarmed mid-session: report the transport as gone. KodiClient's
        // next reconcile Call() fails, it closes and reconnects, and
        // ResolveTarget() (Browse() no longer advertising the fake) sends
        // it back to kDisconnected.
        if (!open_ || !armed_.load()) {
            return std::nullopt;
        }
        // Replies are only handed to a blocking read (a Call() waiting on
        // its id-matched reply), never to the 0 ms pump drain - which
        // exists for unsolicited notifications, and this fake pushes
        // none. Same split as tests/kodi_client_test.cpp's fake.
        if (timeout_ms != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!replies_.empty()) {
                std::string frame = std::move(replies_.front());
                replies_.pop_front();
                return frame;
            }
        }
        return std::nullopt;
    }

    bool IsOpen() const override {
        if (!fake_) {
            return real_ && real_->IsOpen();
        }
        return open_ && armed_.load();
    }

    void Close() override {
        if (real_) {
            real_->Close();
        }
        open_ = false;
    }

private:
    const std::atomic<bool>& armed_;
    bool fake_ = false;
    bool open_ = false;
    std::unique_ptr<HostWebSocketClient> real_;
    std::mutex mutex_;
    std::deque<std::string> replies_;
};

}  // namespace

std::vector<MdnsService> DebugKodiBackend::Browse(const std::string& service_type,
                                                 std::chrono::milliseconds timeout) {
    if (armed_.load() && service_type == KodiClient::kServiceType) {
        MdnsService fake;
        fake.instance_name = "Simulated Kodi";
        fake.address = kFakeHost;
        fake.port = kFakePort;
        fake.txt["uuid"] = kFakeUuid;
        return {std::move(fake)};
    }
    return real_browser_.Browse(service_type, timeout);
}

std::unique_ptr<WebSocketClient> DebugKodiBackend::MakeWebSocketClient() {
    return std::make_unique<DebugKodiWebSocketClient>(armed_);
}

void DebugKodiBackend::Toggle() {
    armed_.store(!armed_.load());
    if (client_ != nullptr) {
        client_->TriggerReconnect();
    }
}

}  // namespace homedeck::sim
