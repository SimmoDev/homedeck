#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace homedeck {

// One service instance returned by an mDNS/DNS-SD browse: the resolved
// SRV target plus the instance's TXT key/value pairs. `address` is a
// resolved IP literal when the backend obtained one (the firmware mdns
// query returns addresses inline; Avahi resolves them on the host);
// `hostname` is the SRV target name (e.g. "Kitchen.local") as a
// fallback. A caller building a URL should prefer `address` when it is
// non-empty - `.local` name resolution is not guaranteed on either
// target's outbound HTTP/WebSocket path.
struct MdnsService {
    std::string instance_name;
    std::string hostname;
    std::string address;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
};

// A one-shot mDNS/DNS-SD service browse - see
// docs/architecture/networking.md's "LAN discovery" section, ADR-0006,
// and ADR-0030 (Kodi, the first consumer).
//
// Deliberately not a universal discovery abstraction and not a
// subscription/callback API: it browses one service type, waits a
// bounded window, and returns whatever resolved. A module's own
// background Task calls this on its connect cycle and acts on the
// result - the same blocking shape WebSocketClient
// (platform/websocket_client.h) already uses, driven by a Task the
// caller owns.
//
// Always returns every instance that answered, never a single "the"
// result: multiple instances of one service type on a LAN (e.g. a Kodi
// box per room) is expected, and choosing between them is the caller's
// policy (ADR-0030), not this layer's.
class MdnsBrowser {
public:
    virtual ~MdnsBrowser() = default;

    // Blocks up to `timeout` collecting answers for `service_type`,
    // given in DNS-SD "<service>.<protocol>" form (e.g.
    // "_xbmc-jsonrpc._tcp"). Returns the instances resolved within the
    // window - empty on a clean timeout with no answers and on any
    // error alike (a caller cannot tell "nothing there" from "browse
    // failed", the same success-only granularity HttpClient /
    // WebSocketClient already expose). Result order is unspecified and
    // not stable between calls.
    virtual std::vector<MdnsService> Browse(const std::string& service_type,
                                            std::chrono::milliseconds timeout) = 0;
};

}  // namespace homedeck
