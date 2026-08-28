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
// Stateless: no lifecycle of its own beyond mdns_init(), which the
// entry point already calls once at startup. Each Browse() is an
// independent bounded query.
class FirmwareMdnsBrowser : public MdnsBrowser {
public:
    std::vector<MdnsService> Browse(const std::string& service_type,
                                    std::chrono::milliseconds timeout) override;
};

}  // namespace homedeck
