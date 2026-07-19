#include "platform/static_assets.h"

#include <utility>

namespace homedeck {

void ServeStaticFiles(HttpServer& server, std::vector<StaticAsset> assets) {
    for (auto& asset : assets) {
        std::string path = asset.path;
        HttpResponse response{200, std::move(asset.content_type), std::move(asset.body), {}};
        server.RegisterHandler(HttpMethod::kGet, path, [response](const HttpRequest&) { return response; });
    }
}

}  // namespace homedeck
