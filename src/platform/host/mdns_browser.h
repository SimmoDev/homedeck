#pragma once

#include "platform/mdns_browser.h"

namespace homedeck {

// Backs MdnsBrowser with libavahi-client (system-linked, the same
// host-only-tooling precedent as HostHttpClient's libcurl - see
// DEVELOPMENT.md).
//
// Built only into the simulator target, not homedeck_platform_host: the
// tests/ build must stay free of this system dependency (the same
// reason HostAudioOutput / UiTask live in the simulator-only
// homedeck_ui target), and KodiClient's own tests exercise the
// MdnsBrowser interface through a fake rather than this backend.
//
// Each Browse() creates its own AvahiClient + service browser, pumps
// the Avahi event loop until the timeout, resolves every instance seen,
// then tears the client down - a one-shot query, not a persistent
// subscription, matching the interface contract. Returns empty (not an
// error) when avahi-daemon isn't running, so the simulator still starts
// on a machine without it.
class HostMdnsBrowser : public MdnsBrowser {
public:
    std::vector<MdnsService> Browse(const std::string& service_type,
                                    std::chrono::milliseconds timeout) override;
};

}  // namespace homedeck
