#pragma once

#include "platform/host/mdns_browser.h"
#include "platform/mdns_browser.h"
#include "platform/websocket_client.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace homedeck {
class KodiClient;
}

namespace homedeck::sim {

// Simulator-only fake Kodi, so the connected-only Kodi Touch UI
// (NowPlayingScreen / KodiRemoteScreen's content_, hidden until
// KodiClient reaches kConnected) can be exercised with no Kodi on the
// LAN - the same role the battery / OTA / power-state buttons in
// debug_panel.cpp play for their own hardware signals.
//
// One object is both the MdnsBrowser handed to KodiClient and the
// factory behind AppCore's make_websocket_client. While disarmed it is
// transparent: Browse() and every WebSocket connect delegate to the
// host backends, so a Kodi on the LAN and Harmony's own hub connection
// are unaffected. Armed:
//   - Browse() returns a single instance at kFakeHost (live discovery
//     suppressed for the duration), so a KodiClient with nothing
//     configured discovers and selects it; and
//   - every Kodi JSON-RPC WebSocket connect (URL path /jsonrpc),
//     however its target was resolved - including a stale manual
//     settings/kodi/host, which ResolveTarget() prefers over discovery -
//     is answered with just enough canned JSON-RPC for the reconcile
//     loop to reach kConnected and render a representative Now Playing.
// Toggling back off drops the fake socket, so KodiClient's next poll
// fails, it reconnects, and (discovery finding nothing, or the
// configured host answering) settles on the actual state again.
//
// The canned player state is static (a video part-way through an
// episode, seekable); the fake does not model Kodi's state machine, so
// a transport/nav button tap sends its JSON-RPC but nothing on screen
// moves in response. Enough to check layout and reachability, not a Kodi
// stand-in.
class DebugKodiBackend : public MdnsBrowser {
public:
    // TEST-NET-1 (RFC 5737) - guaranteed unroutable, so a connect to
    // this host can only be the fake, never an actual box on the LAN.
    static constexpr char kFakeHost[] = "192.0.2.1";
    static constexpr uint16_t kFakePort = 9090;
    static constexpr char kFakeUuid[] = "simulated-kodi";

    std::vector<MdnsService> Browse(const std::string& service_type,
                                    std::chrono::milliseconds timeout) override;

    // Backs AppCore's make_websocket_client factory.
    std::unique_ptr<WebSocketClient> MakeWebSocketClient();

    // Wired once, after AppCore is built. Lets Toggle() poke KodiClient
    // to re-resolve its target immediately rather than waiting for its
    // current connection (possibly to an actual Kodi) to drop on its own.
    void SetClient(KodiClient& client) { client_ = &client; }
    void Toggle();
    bool Armed() const { return armed_.load(); }

private:
    HostMdnsBrowser real_browser_;
    std::atomic<bool> armed_{false};
    KodiClient* client_ = nullptr;
};

}  // namespace homedeck::sim
