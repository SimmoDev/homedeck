#include "platform/host/http_client.h"

#include "platform/host/http_server.h"

#include <gtest/gtest.h>

// A real client/server round trip against a real local HostHttpServer
// instance - hermetic and non-flaky (no live internet dependency),
// while still proving HostHttpClient does genuine network I/O rather
// than exercising only its own internal logic. Matches this project's
// "test for real, not mocked" precedent (AdminAuthService's real
// PBKDF2 test, http_server_test.cpp's own raw-socket round trip).

TEST(HostHttpClient, GetReturnsARealResponseFromARealServer) {
    homedeck::HostHttpServer server;
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/hello", [](const homedeck::HttpRequest&) {
        return homedeck::HttpResponse{200, "text/plain", "world", {}};
    });
    ASSERT_TRUE(server.Start(18184));

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Get("http://127.0.0.1:18184/hello");

    EXPECT_TRUE(response.success);
    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "world");
}

TEST(HostHttpClient, UnregisteredPathReturns404) {
    homedeck::HostHttpServer server;
    server.RegisterHandler(homedeck::HttpMethod::kGet, "/hello", [](const homedeck::HttpRequest&) {
        return homedeck::HttpResponse{200, "text/plain", "world", {}};
    });
    ASSERT_TRUE(server.Start(18185));

    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Get("http://127.0.0.1:18185/nope");

    // A 404 is still a real, received response - success is about the
    // transport, not the HTTP-level outcome (see http_client.h).
    EXPECT_TRUE(response.success);
    EXPECT_EQ(response.status_code, 404);
}

TEST(HostHttpClient, UnreachableServerReturnsFailure) {
    // Nothing listening on this port - a real connection-refused case,
    // not a timeout (which would make this test slow).
    homedeck::HostHttpClient client;
    homedeck::HttpClientResponse response = client.Get("http://127.0.0.1:18186/anything");

    EXPECT_FALSE(response.success);
    EXPECT_EQ(response.status_code, 0);
}
