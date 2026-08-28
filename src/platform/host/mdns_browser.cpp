#include "platform/host/mdns_browser.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <algorithm>
#include <set>

namespace homedeck {

namespace {

// Bounds the work against a misbehaving or hostile responder on an
// unauthenticated LAN, the same posture kMaxWebSocketMessageBytes
// (platform/websocket_client.h) takes.
constexpr size_t kMaxResults = 50;

struct BrowseContext {
    std::vector<MdnsService> results;
    AvahiClient* client = nullptr;
    AvahiSimplePoll* poll = nullptr;
    std::set<AvahiServiceResolver*> pending_resolvers;
};

void ResolveCallback(AvahiServiceResolver* resolver, AvahiIfIndex, AvahiProtocol,
                     AvahiResolverEvent event, const char* name, const char*, const char*,
                     const char* host_name, const AvahiAddress* address, uint16_t port,
                     AvahiStringList* txt, AvahiLookupResultFlags, void* userdata) {
    auto* ctx = static_cast<BrowseContext*>(userdata);

    if (event == AVAHI_RESOLVER_FOUND && ctx->results.size() < kMaxResults) {
        MdnsService svc;
        if (name != nullptr) {
            svc.instance_name = name;
        }
        if (host_name != nullptr) {
            svc.hostname = host_name;
        }
        svc.port = port;
        if (address != nullptr) {
            char buf[AVAHI_ADDRESS_STR_MAX] = {0};
            avahi_address_snprint(buf, sizeof(buf), address);
            svc.address = buf;
        }
        for (AvahiStringList* l = txt; l != nullptr; l = avahi_string_list_get_next(l)) {
            char* key = nullptr;
            char* value = nullptr;
            size_t value_size = 0;
            if (avahi_string_list_get_pair(l, &key, &value, &value_size) == 0) {
                svc.txt.emplace(key != nullptr ? key : "",
                                value != nullptr ? std::string(value, value_size) : "");
                avahi_free(key);
                avahi_free(value);
            }
        }
        ctx->results.push_back(std::move(svc));
    }

    ctx->pending_resolvers.erase(resolver);
    avahi_service_resolver_free(resolver);
}

void BrowseCallback(AvahiServiceBrowser*, AvahiIfIndex iface, AvahiProtocol proto,
                    AvahiBrowserEvent event, const char* name, const char* type,
                    const char* domain, AvahiLookupResultFlags, void* userdata) {
    auto* ctx = static_cast<BrowseContext*>(userdata);

    switch (event) {
        case AVAHI_BROWSER_NEW: {
            if (ctx->pending_resolvers.size() + ctx->results.size() >= kMaxResults) {
                break;
            }
            AvahiServiceResolver* resolver = avahi_service_resolver_new(
                ctx->client, iface, proto, name, type, domain, AVAHI_PROTO_UNSPEC,
                static_cast<AvahiLookupFlags>(0), ResolveCallback, ctx);
            if (resolver != nullptr) {
                ctx->pending_resolvers.insert(resolver);
            }
            break;
        }
        case AVAHI_BROWSER_FAILURE:
            avahi_simple_poll_quit(ctx->poll);
            break;
        default:
            // REMOVE / CACHE_EXHAUSTED / ALL_FOR_NOW - nothing to do for
            // a bounded one-shot browse; the deadline loop below ends it.
            break;
    }
}

void ClientCallback(AvahiClient*, AvahiClientState state, void* userdata) {
    auto* ctx = static_cast<BrowseContext*>(userdata);
    if (state == AVAHI_CLIENT_FAILURE) {
        avahi_simple_poll_quit(ctx->poll);
    }
}

}  // namespace

std::vector<MdnsService> HostMdnsBrowser::Browse(const std::string& service_type,
                                                std::chrono::milliseconds timeout) {
    BrowseContext ctx;

    ctx.poll = avahi_simple_poll_new();
    if (ctx.poll == nullptr) {
        return {};
    }

    int error = 0;
    ctx.client = avahi_client_new(avahi_simple_poll_get(ctx.poll), static_cast<AvahiClientFlags>(0),
                                  ClientCallback, &ctx, &error);
    if (ctx.client == nullptr) {
        avahi_simple_poll_free(ctx.poll);
        return {};
    }

    AvahiServiceBrowser* browser = avahi_service_browser_new(
        ctx.client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, service_type.c_str(), nullptr,
        static_cast<AvahiLookupFlags>(0), BrowseCallback, &ctx);

    if (browser != nullptr) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            // Re-check the deadline a few times a second rather than
            // blocking the whole window in one iterate() call, so a
            // burst of answers early doesn't keep us waiting the full
            // timeout with nothing left to do.
            const int slice_ms = static_cast<int>(std::min<std::int64_t>(remaining, 250));
            // Non-zero => quit requested (browser/client failure) or an
            // internal error; either way, stop pumping.
            if (avahi_simple_poll_iterate(ctx.poll, slice_ms) != 0) {
                break;
            }
        }

        for (AvahiServiceResolver* resolver : ctx.pending_resolvers) {
            avahi_service_resolver_free(resolver);
        }
        avahi_service_browser_free(browser);
    }

    avahi_client_free(ctx.client);
    avahi_simple_poll_free(ctx.poll);

    return std::move(ctx.results);
}

}  // namespace homedeck
