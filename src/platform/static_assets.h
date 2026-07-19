#pragma once

#include "platform/http_server.h"

#include <string>
#include <vector>

namespace homedeck {

// A single Web UI static file - path, content-type, and body. Callers
// build the list differently per target (firmware: EMBED_FILES-linked
// flash data, copied once into `body` at startup - see
// firmware/main/homedeck.cpp; simulator: read from webui/ on disk at
// startup - see simulator/main.cpp) but the serving mechanism itself is
// portable - see
// docs/decisions/ADR-0002-technology-stack.md#6-web-management-ui-static-asset-storage
// for why assets are embedded/loaded once rather than served from a
// filesystem/partition.
struct StaticAsset {
    std::string path;
    std::string content_type;
    std::string body;
};

// Registers one exact-path GET handler per asset via
// server.RegisterHandler(), each unconditionally returning that asset's
// body. Must be called before server.Start(), same contract as
// RegisterHandler() itself.
void ServeStaticFiles(HttpServer& server, std::vector<StaticAsset> assets);

}  // namespace homedeck
