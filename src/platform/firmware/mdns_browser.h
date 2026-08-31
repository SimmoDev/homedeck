#pragma once

#include "platform/mdns_browser.h"

namespace homedeck {

// Backs MdnsBrowser with ESP-IDF's `mdns` component query API
// (mdns_query_ptr, which resolves PTR -> SRV/TXT/A in one call). This is
// the same component firmware/main/homedeck.cpp already uses to
// advertise the device; this is the browse direction, deferred from
// M2's LAN discovery item until a module needed it (Kodi, M4 - see
// docs/architecture/networking.md).
//
// Stateless: each Browse() calls mdns_init() itself (idempotent - the
// entry point also brings the same component up to advertise the device)
// and then runs one independent bounded query, so it works regardless of
// whether the advertisement path has run yet.
class FirmwareMdnsBrowser : public MdnsBrowser {
public:
    std::vector<MdnsService> Browse(const std::string& service_type,
                                    std::chrono::milliseconds timeout) override;
};

}  // namespace homedeck
