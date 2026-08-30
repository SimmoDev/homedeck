#include "platform/firmware/mdns_browser.h"

#include "esp_netif.h"
#include "mdns.h"

#include <algorithm>

namespace homedeck {

namespace {

// A LAN will not sanely run more instances of one service type than
// this; bounds the result walk against a misbehaving or hostile
// responder, the same defensive posture kMaxWebSocketMessageBytes takes
// (platform/websocket_client.h) for an unauthenticated LAN peer.
// HostMdnsBrowser uses the same cap.
constexpr size_t kMaxResults = 20;

}  // namespace

std::vector<MdnsService> FirmwareMdnsBrowser::Browse(const std::string& service_type,
                                                    std::chrono::milliseconds timeout) {
    std::vector<MdnsService> out;

    // mdns_query_ptr() takes the service and protocol as separate
    // labels ("_xbmc-jsonrpc", "_tcp"); the interface takes the joined
    // DNS-SD form ("_xbmc-jsonrpc._tcp"). Split at the last '.'.
    const std::string::size_type dot = service_type.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= service_type.size()) {
        return out;
    }
    const std::string service = service_type.substr(0, dot);
    const std::string proto = service_type.substr(dot + 1);

    const uint32_t timeout_ms =
        static_cast<uint32_t>(std::clamp<long long>(timeout.count(), 1, 60000));

    mdns_result_t* results = nullptr;
    if (mdns_query_ptr(service.c_str(), proto.c_str(), timeout_ms, kMaxResults, &results) != ESP_OK) {
        return out;
    }

    for (mdns_result_t* r = results; r != nullptr; r = r->next) {
        MdnsService svc;
        if (r->instance_name != nullptr) {
            svc.instance_name = r->instance_name;
        }
        if (r->hostname != nullptr) {
            svc.hostname = r->hostname;
        }
        svc.port = r->port;

        if (r->addr != nullptr && r->addr->addr.type == ESP_IPADDR_TYPE_V4) {
            char buf[16] = {0};
            // esp_ip4addr_ntoa() returns nullptr if buf is too small to
            // hold the formatted address - shouldn't happen (16 bytes is
            // the exact max IPv4 string length, "255.255.255.255\0"), but
            // checked rather than assumed; svc.address stays empty on
            // failure and falls back to `hostname` below, the same path
            // an IPv6-only instance already takes.
            if (esp_ip4addr_ntoa(&r->addr->addr.u_addr.ip4, buf, sizeof(buf)) != nullptr) {
                svc.address = buf;
            }
        }
        // IPv6-only instances fall back to `hostname` - no Kodi/Home
        // Assistant setup this targets is v6-only, and adding an lwip
        // inet_ntop path for a case nothing exercises isn't worth it.

        for (size_t i = 0; i < r->txt_count; ++i) {
            const char* key = r->txt[i].key;
            if (key == nullptr) {
                continue;
            }
            const char* value = r->txt[i].value;
            svc.txt.emplace(key, value != nullptr ? value : "");
        }

        out.push_back(std::move(svc));
    }

    mdns_query_results_free(results);
    return out;
}

}  // namespace homedeck
