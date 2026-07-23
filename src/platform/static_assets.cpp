#include "platform/static_assets.h"

#include <utility>

namespace homedeck {

void ServeStaticFiles(HttpServer& server, std::vector<StaticAsset> assets) {
    for (auto& asset : assets) {
        std::string path = asset.path;
        HttpResponse response{200, std::move(asset.content_type), std::move(asset.body), {}};
        // The capture itself is a one-time move, not a copy. Each
        // request still copies the body on return (Handler's
        // std::function<HttpResponse(...)> contract requires a real
        // HttpResponse by value) - avoiding that too would mean
        // HttpResponse sharing its body via shared_ptr instead of
        // owning a plain std::string, rippling into every
        // RegisterHandler call site in the codebase for a cost (a few
        // tens of KB, on a LAN-only device serving one browser at a
        // time) not worth that blast radius.
        server.RegisterHandler(HttpMethod::kGet, path,
                                [response = std::move(response)](const HttpRequest&) { return response; });
    }
}

}  // namespace homedeck
