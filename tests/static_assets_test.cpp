#include "platform/static_assets.h"

#include "platform/host/http_server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace {

// Same minimal real HTTP/1.1 client as http_server_test.cpp's HttpGet -
// duplicated rather than shared, matching admin_auth_routes_test.cpp's
// own precedent for this exact tradeoff.
std::string HttpGet(uint16_t port, const std::string& path) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return "";
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(sock);
        return "";
    }

    std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    send(sock, request.data(), request.size(), 0);

    std::string response;
    char buffer[4096];
    ssize_t received;
    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, static_cast<size_t>(received));
    }
    close(sock);
    return response;
}

}  // namespace

TEST(ServeStaticFiles, RegisteredAssetIsServedWithItsContentType) {
    homedeck::HostHttpServer server;
    homedeck::ServeStaticFiles(server, {{"/", "text/html", "<h1>hi</h1>"}, {"/app.js", "application/javascript", "1"}});
    ASSERT_TRUE(server.Start(0));

    std::string index_response = HttpGet(server.BoundPort(), "/");
    EXPECT_NE(index_response.find("200"), std::string::npos);
    EXPECT_NE(index_response.find("Content-Type: text/html"), std::string::npos);
    EXPECT_NE(index_response.find("<h1>hi</h1>"), std::string::npos);

    std::string js_response = HttpGet(server.BoundPort(), "/app.js");
    EXPECT_NE(js_response.find("Content-Type: application/javascript"), std::string::npos);
}

TEST(ServeStaticFiles, UnregisteredPathStillReturns404) {
    homedeck::HostHttpServer server;
    homedeck::ServeStaticFiles(server, {{"/", "text/html", "<h1>hi</h1>"}});
    ASSERT_TRUE(server.Start(0));

    std::string response = HttpGet(server.BoundPort(), "/nope");

    EXPECT_NE(response.find("404"), std::string::npos);
}
