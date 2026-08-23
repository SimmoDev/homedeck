#include "core/harmony_connection.h"

#include "platform/host/cache_store.h"
#include "platform/host/http_client.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"
#include "platform/host/websocket_client.h"

#include <gtest/gtest.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Same scriptable-response HttpClient double as
// tests/weather_provider_test.cpp's FakeHttpClient, extended with Post()
// (HarmonyConnection's handshake is a POST, never a GET).
class FakeHttpClient : public homedeck::HttpClient {
public:
    homedeck::HttpClientResponse Get(const std::string& /*url*/) override { return homedeck::HttpClientResponse{false, 0, ""}; }

    homedeck::HttpClientResponse Post(const std::string& /*url*/, const std::string& /*json_body*/,
                                       const std::vector<std::pair<std::string, std::string>>& /*extra_headers*/ = {}) override {
        std::lock_guard<std::mutex> lock(mutex_);
        post_count_++;
        return response_;
    }

    void SetResponse(homedeck::HttpClientResponse response) {
        std::lock_guard<std::mutex> lock(mutex_);
        response_ = std::move(response);
    }

    int PostCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return post_count_;
    }

private:
    std::mutex mutex_;
    homedeck::HttpClientResponse response_{false, 0, ""};
    int post_count_ = 0;
};

// Shared control block every FakeWebSocketClient instance this test's
// factory hands out reads/writes through - HarmonyConnection creates a
// fresh WebSocketClient per (re)connect attempt, so a single fake
// instance can't be handed to the constructor the way FakeHttpClient is.
struct WsScript {
    std::mutex mutex;
    bool connect_should_succeed = true;
    bool send_should_succeed = true;
    std::vector<std::string> connect_urls;
    std::vector<std::string> sent_texts;
    std::deque<std::string> responses;
    // Separate from `responses` above - visible only to a 0ms ReceiveText()
    // call (DrainStaleMessages()'s non-blocking poll), modeling a message
    // that arrived before the drain runs. `responses` models this test's
    // own expected reply to whatever request follows the drain, and must
    // stay untouched by it regardless of when the test pushed it relative
    // to the connection loop actually running - a real backend's 0ms
    // receive only ever sees what's already buffered, never something a
    // later, bounded-timeout receive is the one meant to wait for.
    std::deque<std::string> stale_responses;
    int close_count = 0;
    // Ordered log of "send:<text>" / "receive0" (a 0ms/drain poll) /
    // "receiveN" (a bounded-timeout receive) entries - unlike the
    // separate responses/stale_responses queues above (which can't tell
    // a test whether a drain happened *before* a particular send, only
    // whether one happened at all), this proves ordering: that
    // DrainStaleMessages() runs before a specific request is sent, not
    // just that it runs somewhere.
    std::vector<std::string> call_log;
    // Lets a test pause the connection loop's own thread mid-SendText(),
    // after recording the call but before returning - the only way to
    // deterministically land a Stop() between two commands in the same
    // SendPendingCommands() batch, which otherwise has no observable sync
    // point to aim at (see StopDuringAnInFlightBatchSendPublishesADroppedEvent).
    bool block_next_send = false;
    bool send_blocked = false;
    bool release_blocked_send = false;
    std::condition_variable send_cv;
};

class FakeWebSocketClient : public homedeck::WebSocketClient {
public:
    explicit FakeWebSocketClient(std::shared_ptr<WsScript> script) : script_(std::move(script)) {}

    bool Connect(const std::string& url) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->connect_urls.push_back(url);
        return script_->connect_should_succeed;
    }

    bool SendText(const std::string& text) override {
        std::unique_lock<std::mutex> lock(script_->mutex);
        script_->sent_texts.push_back(text);
        script_->call_log.push_back("send:" + text);
        if (script_->block_next_send) {
            script_->block_next_send = false;
            script_->send_blocked = true;
            script_->send_cv.notify_all();
            script_->send_cv.wait(lock, [this] { return script_->release_blocked_send; });
        }
        return script_->send_should_succeed;
    }

    std::optional<std::string> ReceiveText(int timeout_ms) override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        if (timeout_ms == 0) {
            script_->call_log.push_back("receive0");
            // See WsScript::stale_responses's own comment.
            if (script_->stale_responses.empty()) {
                return std::nullopt;
            }
            std::string response = std::move(script_->stale_responses.front());
            script_->stale_responses.pop_front();
            return response;
        }
        script_->call_log.push_back("receiveN");
        if (script_->responses.empty()) {
            return std::nullopt;
        }
        std::string response = std::move(script_->responses.front());
        script_->responses.pop_front();
        return response;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->close_count++;
    }

private:
    std::shared_ptr<WsScript> script_;
};

// Polls Snapshot()/a predicate from the test thread until it holds or a
// bounded number of short sleeps elapse - same pattern
// weather_provider_test.cpp's WaitFor() uses, since HarmonyConnection's
// state changes happen on its own background Task's thread.
template <typename Predicate>
bool WaitFor(Predicate predicate, int max_attempts = 300) {
    for (int i = 0; i < max_attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

constexpr char kHandshakeSuccessBody[] =
    R"({"id":1,"msg":"OK","data":{"activeRemoteId":17389408,"email":"someone@example.com"}})";

// The device entry's controlGroup/function/action shape matches a config
// payload pulled from the reference hub during this feature's own design
// pass (see ADR-0029's Consequences) - not a guessed shape.
constexpr char kConfigSuccessBody[] =
    R"({"data":{)"
    R"("device":[{"id":"1","label":"TV","controlGroup":[)"
    R"({"name":"Power","function":[{"name":"PowerToggle","label":"Power Toggle","action":"{\"command\":\"PowerToggle\",\"type\":\"IRCommand\",\"deviceId\":\"1\"}"}]},)"
    R"({"name":"Volume","function":[)"
    R"({"name":"VolumeUp","label":"VolumeUp","action":"{\"command\":\"VolumeUp\",\"type\":\"IRCommand\",\"deviceId\":\"1\"}"},)"
    R"({"name":"VolumeDown","label":"VolumeDown","action":"{\"command\":\"VolumeDown\",\"type\":\"IRCommand\",\"deviceId\":\"1\"}"})"
    R"(]})"
    R"(]}],)"
    R"("activity":[{"id":"-1","label":"Off"},{"id":"123","label":"Watch TV"}])"
    R"(}})";

std::string CurrentActivityResponseBody(const std::string& activity_id) {
    return R"({"data":{"result":")" + activity_id + R"("}})";
}

void PushResponse(const std::shared_ptr<WsScript>& script, std::string response) {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->responses.push_back(std::move(response));
}

// See WsScript::stale_responses's own comment.
void PushStaleResponse(const std::shared_ptr<WsScript>& script, std::string response) {
    std::lock_guard<std::mutex> lock(script->mutex);
    script->stale_responses.push_back(std::move(response));
}

class HarmonyConnectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_harmony_connection_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
};

constexpr std::chrono::milliseconds kFastBackoff = std::chrono::milliseconds(30);

// A minimal raw-socket stand-in for a real Harmony hub's own HTTP-
// handshake-then-WebSocket sequence (ADR-0029), used by
// RealBackendConnectsHandshakesAndFetchesConfigOverActualSocketsAndFraming
// below - every other test in this file drives HarmonyConnection against
// FakeHttpClient/FakeWebSocketClient, scriptable doubles that can't
// reproduce real socket/timing behavior (e.g. DrainStaleMessages()'s own
// 0ms ReceiveText() poll, which depends on what a real backend actually
// does with a zero timeout - see HostWebSocketClient's own
// ReceiveTextZeroDoesNotBlockWhenNothingIsPending regression test in
// websocket_client_test.cpp, added after that exact gap let a real bug
// through undetected). Deliberately minimal - just enough of HTTP/1.1
// and RFC 6455 framing to complete one connect pipeline, not a general-
// purpose test server - same scope websocket_client_test.cpp's own
// helpers keep. Not shared with that file: each raw-socket test file in
// this project builds its own minimal peer locally rather than a shared
// abstraction serving two very different protocols end-to-end.
//
// HandshakeUrl()/WebSocketUrl() (harmony_connection.cpp) hardcode port
// 8088 - the real hub's own port, and hub_host itself carries no port
// (IsValidHubHost() rejects ':') - so this fake hub has no choice but to
// listen there too.
constexpr int kFakeHubPort = 8088;

std::string ReadRawHttpRequest(int fd) {
    std::string data;
    char buf[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
    }
    return data;
}

std::string ExtractWsHeaderValue(const std::string& request, const std::string& header_name) {
    size_t pos = request.find(header_name + ": ");
    if (pos == std::string::npos) return "";
    pos += header_name.size() + 2;
    size_t end = request.find("\r\n", pos);
    return request.substr(pos, end - pos);
}

std::string ComputeWsAcceptKey(const std::string& client_key) {
    static constexpr char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string combined = client_key + kGuid;
    unsigned char digest[20];
    mbedtls_sha1(reinterpret_cast<const unsigned char*>(combined.data()), combined.size(), digest);
    unsigned char encoded[64];
    size_t out_len = 0;
    mbedtls_base64_encode(encoded, sizeof(encoded), &out_len, digest, sizeof(digest));
    return std::string(reinterpret_cast<char*>(encoded), out_len);
}

// Reads one client->server WS frame (RFC 6455 requires client frames
// masked) and returns its unmasked payload. Single-frame messages only -
// every request HarmonyConnection sends fits in one.
std::string ReadWsClientTextFrame(int fd) {
    unsigned char header[2];
    if (read(fd, header, 2) != 2) return "";
    bool masked = (header[1] & 0x80) != 0;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (read(fd, ext, 2) != 2) return "";
        len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (read(fd, ext, 8) != 8) return "";
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
    }
    unsigned char mask[4] = {};
    if (masked && read(fd, mask, 4) != 4) return "";
    std::string payload(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, &payload[got], len - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    if (masked) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
    }
    return payload;
}

// Sends one small, complete server->client WS text frame (never masked,
// per RFC 6455). A plain blocking write is fine - every reply here is a
// few hundred bytes at most.
void SendWsServerTextFrame(int fd, const std::string& payload) {
    std::vector<unsigned char> header;
    header.push_back(0x81);  // FIN=1, opcode=text
    if (payload.size() < 126) {
        header.push_back(static_cast<unsigned char>(payload.size()));
    } else {
        header.push_back(126);
        header.push_back(static_cast<unsigned char>((payload.size() >> 8) & 0xFF));
        header.push_back(static_cast<unsigned char>(payload.size() & 0xFF));
    }
    send(fd, header.data(), header.size(), MSG_NOSIGNAL);
    send(fd, payload.data(), payload.size(), MSG_NOSIGNAL);
}

int ListenOnFakeHubPort() {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(kFakeHubPort);
    bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(listen_fd, 2);
    return listen_fd;
}

// Plays both halves of ADR-0029's protocol over real sockets: the plain-
// HTTP handshake POST (a separate TCP connection, matching a real hub -
// HandshakeUrl()'s http:// scheme and WebSocketUrl()'s ws:// scheme are
// never the same connection), then the WS upgrade and the two request/
// reply round trips ConnectAndFetchConfig() makes (config fetch, then
// its own best-effort current-activity probe).
void RunFakeHarmonyHub(int listen_fd) {
    int http_fd = accept(listen_fd, nullptr, nullptr);
    if (http_fd >= 0) {
        ReadRawHttpRequest(http_fd);
        std::string body = kHandshakeSuccessBody;
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
        send(http_fd, response.data(), response.size(), MSG_NOSIGNAL);
        close(http_fd);
    }

    int ws_fd = accept(listen_fd, nullptr, nullptr);
    if (ws_fd < 0) return;
    std::string upgrade_request = ReadRawHttpRequest(ws_fd);
    std::string client_key = ExtractWsHeaderValue(upgrade_request, "Sec-WebSocket-Key");
    std::string upgrade_response = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: " +
                                    ComputeWsAcceptKey(client_key) + "\r\n\r\n";
    send(ws_fd, upgrade_response.data(), upgrade_response.size(), MSG_NOSIGNAL);

    ReadWsClientTextFrame(ws_fd);  // the config fetch request
    SendWsServerTextFrame(ws_fd, kConfigSuccessBody);

    ReadWsClientTextFrame(ws_fd);  // ConnectAndFetchConfig()'s own current-activity probe
    SendWsServerTextFrame(ws_fd, CurrentActivityResponseBody("-1"));

    close(ws_fd);
}

// Extends RunFakeHarmonyHub()'s own sequence with the one real-socket
// scenario it doesn't cover: a stray, unsolicited frame (a reply to some
// earlier request with no matching receive of its own - see
// DrainStaleMessages()'s own comment) sent with no reader waiting on the
// other end, then a real getCurrentActivity reply for the liveness
// probe that follows. Exercises the exact gap
// [[feedback-fake-doubles-hide-backend-timing-bugs]] names:
// FakeWebSocketClient's own ReceiveText(0) always modeled "0ms = check
// what's already buffered" correctly by construction, which proved
// nothing about whether the real backend's 0ms receive actually behaves
// that way against a real kernel socket buffer (it didn't, once - see
// HostWebSocketClient's own ReceiveTextZeroDoesNotBlockWhenNothingIsPending
// regression test in websocket_client_test.cpp). If DrainStaleMessages()
// failed to drain this stray frame against the real backend, the next
// liveness probe's own bounded-timeout receive would read it instead of
// the real reply below.
void RunFakeHarmonyHubWithStaleFrame(int listen_fd) {
    int http_fd = accept(listen_fd, nullptr, nullptr);
    if (http_fd >= 0) {
        ReadRawHttpRequest(http_fd);
        std::string body = kHandshakeSuccessBody;
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
        send(http_fd, response.data(), response.size(), MSG_NOSIGNAL);
        close(http_fd);
    }

    int ws_fd = accept(listen_fd, nullptr, nullptr);
    if (ws_fd < 0) return;
    std::string upgrade_request = ReadRawHttpRequest(ws_fd);
    std::string client_key = ExtractWsHeaderValue(upgrade_request, "Sec-WebSocket-Key");
    std::string upgrade_response = "HTTP/1.1 101 Switching Protocols\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: " +
                                    ComputeWsAcceptKey(client_key) + "\r\n\r\n";
    send(ws_fd, upgrade_response.data(), upgrade_response.size(), MSG_NOSIGNAL);

    ReadWsClientTextFrame(ws_fd);  // the config fetch request
    SendWsServerTextFrame(ws_fd, kConfigSuccessBody);

    ReadWsClientTextFrame(ws_fd);  // ConnectAndFetchConfig()'s own current-activity probe
    SendWsServerTextFrame(ws_fd, CurrentActivityResponseBody("-1"));

    // Sent immediately, well before the client's own next liveness-probe
    // cycle (the test below uses a short liveness_interval) - real
    // enough slack over loopback for this to already be sitting in the
    // client's own socket receive buffer by the time DrainStaleMessages()
    // polls for it.
    SendWsServerTextFrame(ws_fd, R"({"data":{"result":"stray-not-a-real-activity-id"}})");

    ReadWsClientTextFrame(ws_fd);  // the next liveness probe's own getCurrentActivity request
    SendWsServerTextFrame(ws_fd, CurrentActivityResponseBody("123"));

    close(ws_fd);
}

}  // namespace

// Mirrors webui/src/lib/harmonyValidation.test.ts's coverage of
// hubHostError() - the two must reject the same values, see
// IsValidHubHost()'s own comment.
TEST(IsValidHubHostTest, AcceptsAPlainHostnameOrIpAddress) {
    EXPECT_TRUE(homedeck::IsValidHubHost("192.168.1.50"));
    EXPECT_TRUE(homedeck::IsValidHubHost("harmony-hub.local"));
}

TEST(IsValidHubHostTest, AcceptsEmptyNotThisFunctionsConcern) {
    EXPECT_TRUE(homedeck::IsValidHubHost(""));
}

TEST(IsValidHubHostTest, RejectsAValueWithASchemePrefix) {
    EXPECT_FALSE(homedeck::IsValidHubHost("http://192.168.1.50"));
}

TEST(IsValidHubHostTest, RejectsAValueContainingWhitespace) {
    EXPECT_FALSE(homedeck::IsValidHubHost("192.168.1.50 "));
    EXPECT_FALSE(homedeck::IsValidHubHost("192.168 1.50"));
}

TEST(IsValidHubHostTest, RejectsAValueContainingAPath) {
    EXPECT_FALSE(homedeck::IsValidHubHost("192.168.1.50/setup"));
}

TEST(IsValidHubHostTest, RejectsAFullUrlWithBothASchemeAndAPath) {
    EXPECT_FALSE(homedeck::IsValidHubHost("http://192.168.1.50/setup"));
}

TEST(IsValidHubHostTest, RejectsNonAsciiBytesIncludingUnicodeWhitespace) {
    // U+00A0 (NBSP), UTF-8-encoded as 0xC2 0xA0 - matched by the
    // frontend's `/\s/` regex (webui/src/lib/harmonyValidation.ts) but
    // invisible to a byte-wise std::isspace() scan alone; rejecting every
    // non-ASCII byte closes that gap without enumerating Unicode
    // whitespace codepoints one by one.
    EXPECT_FALSE(homedeck::IsValidHubHost("192.168.1.50\xC2\xA0"));
}

TEST(IsValidHubHostTest, RejectsNonWhitespaceControlCharacters) {
    // \x01 - not whitespace, not the high bit, so it needs its own check,
    // matching webui/src/lib/harmonyValidation.ts's identical
    // control-character check.
    EXPECT_FALSE(homedeck::IsValidHubHost(std::string("192.168.1.50") + '\x01'));
}

TEST(IsValidHubHostTest, RejectsUrlStructuralCharacters) {
    // #/?/@ all pass every other check (no scheme, no whitespace, no
    // path, plain ASCII) but change what HandshakeUrl()/WebSocketUrl()'s
    // raw concatenation actually connects to instead of just failing to
    // connect - see IsValidHubHost()'s own comment.
    EXPECT_FALSE(homedeck::IsValidHubHost("realhost#fragment"));
    EXPECT_FALSE(homedeck::IsValidHubHost("realhost?query=1"));
    EXPECT_FALSE(homedeck::IsValidHubHost("user@realhost"));
}

TEST(IsValidHubHostTest, RejectsABareUnbracketedIpv6Literal) {
    // "::1"/"fe80::1" pass every other check (no scheme, no whitespace,
    // no path, no #?@, plain ASCII) but RFC 3986 requires an IPv6
    // literal to be bracketed in a URL authority - unbracketed, it's
    // ambiguous to the URL parser HandshakeUrl()/WebSocketUrl()'s raw
    // concatenation ultimately feeds.
    EXPECT_FALSE(homedeck::IsValidHubHost("::1"));
    EXPECT_FALSE(homedeck::IsValidHubHost("fe80::1"));
}

TEST_F(HarmonyConnectionTest, NotConfiguredStaysDisconnectedAndNeverCallsOut) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    homedeck::EventBus bus;
    FakeHttpClient http_client;
    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kDisconnected);
    EXPECT_EQ(http_client.PostCount(), 0);
    connection.Stop();
}

TEST_F(HarmonyConnectionTest, ConfiguredHandshakeAndConfigFetchSucceedPublishesConnectedWithDevicesAndActivities) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
        // Consumed by ConnectAndFetchConfig()'s own best-effort current-
        // activity fetch, immediately after config succeeds - without
        // this, that probe's ReceiveText() finds no response queued
        // (a transport failure, not a parse one) and the whole connect
        // attempt now fails (see ConnectAndFetchConfig()'s own comment),
        // which isn't this test's own concern.
        script->responses.push_back(CurrentActivityResponseBody("-1"));
    }

    std::atomic<int> connected_events{0};
    std::atomic<int> config_events{0};
    auto state_sub = bus.Subscribe<homedeck::HarmonyConnectionStateChangedEvent>(
        [&connected_events](const homedeck::HarmonyConnectionStateChangedEvent& event) {
            if (event.state == homedeck::HarmonyConnectionState::kConnected) connected_events++;
        });
    auto config_sub = bus.Subscribe<homedeck::HarmonyConfigUpdatedEvent>(
        [&config_events](const homedeck::HarmonyConfigUpdatedEvent&) { config_events++; });

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_TRUE(WaitFor([&] { return connected_events.load() > 0; }));
    ASSERT_TRUE(WaitFor([&] { return config_events.load() > 0; }));

    homedeck::HarmonyConnectionSnapshot snapshot = connection.Snapshot();
    EXPECT_EQ(snapshot.state, homedeck::HarmonyConnectionState::kConnected);
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].id, "1");
    EXPECT_EQ(snapshot.devices[0].label, "TV");
    ASSERT_EQ(snapshot.devices[0].control_groups.size(), 2u);
    EXPECT_EQ(snapshot.devices[0].control_groups[0].name, "Power");
    ASSERT_EQ(snapshot.devices[0].control_groups[0].commands.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].control_groups[0].commands[0].name, "PowerToggle");
    EXPECT_EQ(snapshot.devices[0].control_groups[0].commands[0].label, "Power Toggle");
    EXPECT_EQ(snapshot.devices[0].control_groups[0].commands[0].action,
              R"({"command":"PowerToggle","type":"IRCommand","deviceId":"1"})");
    EXPECT_EQ(snapshot.devices[0].control_groups[1].name, "Volume");
    ASSERT_EQ(snapshot.devices[0].control_groups[1].commands.size(), 2u);
    EXPECT_EQ(snapshot.devices[0].control_groups[1].commands[0].name, "VolumeUp");
    EXPECT_EQ(snapshot.devices[0].control_groups[1].commands[1].name, "VolumeDown");
    ASSERT_EQ(snapshot.activities.size(), 2u);
    EXPECT_EQ(snapshot.activities[1].id, "123");
    EXPECT_EQ(snapshot.activities[1].label, "Watch TV");

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        ASSERT_FALSE(script->connect_urls.empty());
        EXPECT_NE(script->connect_urls.front().find("hubId=17389408"), std::string::npos);
    }

    connection.Stop();
}

// A device/activity entry with a present-but-wrong-typed id (here, null -
// neither a string nor a number) must be dropped rather than kept with an
// empty id - an empty id is otherwise a real, tappable button whose
// StartActivity("")/command send the hub has no defined behavior for.
// This protocol has no authentication (see ADR-0029), so a malformed
// field from a spoofed hub isn't purely hypothetical.
TEST_F(HarmonyConnectionTest, EntriesWithAWrongTypedIdAreDroppedNotKeptEmpty) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script,
                 R"({"data":{)"
                 R"("device":[{"id":null,"label":"Bad"},{"id":"1","label":"TV"}],)"
                 R"("activity":[{"id":[1,2],"label":"Bad"},{"id":"123","label":"Watch TV"}])"
                 R"(}})");

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    homedeck::HarmonyConnectionSnapshot snapshot = connection.Snapshot();
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].id, "1");
    ASSERT_EQ(snapshot.activities.size(), 1u);
    EXPECT_EQ(snapshot.activities[0].id, "123");

    connection.Stop();
}

// Saving hub_host back to empty - the Web UI's "clear the configured hub"
// flow, HarmonySettings.svelte - must actually clear
// devices/activities/current_activity_id/has_config, not just move state
// to kDisconnected while leaving ActivitiesScreen/DevicesScreen (both
// keyed on has_config alone, not state) still rendering the previous
// hub's stale, tappable list forever.
TEST_F(HarmonyConnectionTest, UnconfiguringAfterConnectingClearsTheCachedConfig) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
    }

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_FALSE(connection.Snapshot().devices.empty());
    ASSERT_FALSE(connection.Snapshot().activities.empty());

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, ""));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return !connection.Snapshot().has_config; }));
    homedeck::HarmonyConnectionSnapshot snapshot = connection.Snapshot();
    EXPECT_EQ(snapshot.state, homedeck::HarmonyConnectionState::kDisconnected);
    EXPECT_TRUE(snapshot.devices.empty());
    EXPECT_TRUE(snapshot.activities.empty());
    EXPECT_TRUE(snapshot.current_activity_id.empty());

    connection.Stop();
}

// Distinct from UnconfiguringAfterConnectingClearsTheCachedConfig above -
// this is hub_host changing to a *different* non-empty address, not
// clearing it. The new connect attempt is made to fail here specifically
// so has_config going false can only be explained by the address change
// itself, not by a coincidentally-successful reconnect racing the
// assertion - a bare failed reconnect to the *same* host must NOT clear
// has_config (see HarmonyConnectionSnapshot::has_config's own comment),
// so this is the one case that tells the two apart.
TEST_F(HarmonyConnectionTest, ChangingHubHostToADifferentAddressClearsTheCachedConfig) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    // Consumed by ConnectAndFetchConfig()'s own best-effort current-
    // activity fetch, immediately after config succeeds - see
    // ConfiguredHandshakeAndConfigFetchSucceedPublishesConnectedWithDevicesAndActivities's
    // identical comment.
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_FALSE(connection.Snapshot().devices.empty());

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->connect_should_succeed = false;
    }
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "192.168.1.99"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    homedeck::HarmonyConnectionSnapshot snapshot = connection.Snapshot();
    EXPECT_FALSE(snapshot.has_config) << "the old hub's config must not linger once pointed at a different address";
    EXPECT_TRUE(snapshot.devices.empty());
    EXPECT_TRUE(snapshot.activities.empty());

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, ChangingHubHostAfterANeverSuccessfulAttemptStillPublishesConfigUpdated) {
    // Neither address here ever reaches has_config=true - unlike
    // ChangingHubHostToADifferentAddressClearsTheCachedConfig above, this
    // covers ClearConfigIfPresent()'s force_publish path specifically:
    // HarmonyConfigUpdatedEvent must still fire on the address change so
    // HarmonyNotificationBridge can tell "a fresh address's own first
    // failure" apart from "the previous address's already-notified one",
    // even though there was no cached config to actually clear.
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{false, 0, ""});

    std::atomic<int> config_events{0};
    auto config_sub = bus.Subscribe<homedeck::HarmonyConfigUpdatedEvent>(
        [&config_events](const homedeck::HarmonyConfigUpdatedEvent&) { config_events++; });

    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    int events_before_change = config_events.load();
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "192.168.1.99"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return config_events.load() > events_before_change; }));

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, ChangingHubHostDropsPendingCommandsQueuedAgainstThePreviousHub) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    // Hub A's handshake never succeeds, so a command queued while pointed
    // at it stays queued (kError's backoff Sleep() doesn't watch commands
    // - see Sleep()'s own watch_commands comment) rather than ever being
    // sent to hub A itself.
    http_client.SetResponse(homedeck::HttpClientResponse{false, 0, ""});

    std::atomic<int> dropped_events{0};
    auto dropped_sub =
        bus.Subscribe<homedeck::HarmonyCommandDroppedEvent>([&](const homedeck::HarmonyCommandDroppedEvent&) { dropped_events++; });

    auto script = std::make_shared<WsScript>();
    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    connection.PressDeviceCommand(R"({"command":"hubA_only"})");

    // Point at hub B, which does connect successfully - this must not let
    // the still-queued hub-A command reach hub B using hub B's own hub_id_.
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "192.168.1.99"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        for (const std::string& sent : script->sent_texts) {
            EXPECT_EQ(sent.find("hubA_only"), std::string::npos)
                << "a command queued against the previous hub must never reach the newly-configured one";
        }
    }
    EXPECT_GE(dropped_events.load(), 1) << "the abandoned hub-A command should have published a drop event";

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, HandshakeFailureEntersErrorStateThenRecoversOnceItSucceeds) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{false, 0, ""});  // handshake fails

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
        // Consumed by ConnectAndFetchConfig()'s own best-effort current-
        // activity fetch, immediately after config succeeds - see
        // ConfiguredHandshakeAndConfigFetchSucceedPublishesConnectedWithDevicesAndActivities's
        // identical comment.
        script->responses.push_back(CurrentActivityResponseBody("-1"));
    }

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    // Fix the handshake - the retry loop (kFastBackoff later) should pick
    // it up on its own without any external trigger.
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    EXPECT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    connection.Stop();
}

// Distinct from HandshakeFailureEntersErrorStateThenRecoversOnceItSucceeds
// above (a transport-level failure) - this is a 200 response whose body
// isn't valid JSON at all. ConnectAndFetchConfig()'s parse guard
// (parsed.is_discarded()) must reject it the same way, not crash or
// misbehave.
TEST_F(HarmonyConnectionTest, HandshakeWithMalformedJsonBodyEntersErrorState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, "not valid json{{{"});

    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    connection.Stop();
}

// A well-formed but excessively-nested JSON body (deeper than
// ExceedsJsonNestingDepth() allows) must be rejected the same way as
// outright malformed JSON, not handed to nlohmann::json::parse() and left
// to recurse arbitrarily deep - this protocol has no authentication
// (ADR-0029), so a rogue device on the LAN could otherwise send this to
// drive a stack overflow.
TEST_F(HarmonyConnectionTest, HandshakeWithExcessivelyNestedJsonBodyEntersErrorState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    std::string nested_body;
    for (int i = 0; i < 40; ++i) nested_body += "[";
    for (int i = 0; i < 40; ++i) nested_body += "]";
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, nested_body});

    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    connection.Stop();
}

// A well-formed JSON object missing the field this class actually needs
// (activeRemoteId) - the hub returning a shape this project doesn't
// recognize must not be treated as success.
TEST_F(HarmonyConnectionTest, HandshakeMissingActiveRemoteIdEntersErrorState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(
        homedeck::HttpClientResponse{true, 200, R"({"id":1,"msg":"OK","data":{"email":"someone@example.com"}})"});

    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    connection.Stop();
}

// activeRemoteId comes from the hub's own (per ADR-0029, unauthenticated)
// handshake response and becomes the trailing hubId value in
// WebSocketUrl()'s raw concatenation - a value containing '&' could
// otherwise inject an extra query parameter into the WebSocket connect
// URL. Must be treated as a malformed handshake, the same as a missing
// field, not silently used as-is.
TEST_F(HarmonyConnectionTest, HandshakeWithNonAlphanumericActiveRemoteIdEntersErrorState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{
        true, 200, R"({"id":1,"msg":"OK","data":{"activeRemoteId":"17389408&evil=1"}})"});

    auto script = std::make_shared<WsScript>();

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->connect_urls.empty());
    }

    connection.Stop();
}

// The handshake succeeds, but the WebSocket config-fetch response isn't
// valid JSON - ConnectAndFetchConfig()'s second parse guard must reject
// it the same way as the handshake's own, leaving ws_client_ closed
// rather than left dangling half-open.
TEST_F(HarmonyConnectionTest, ConfigFetchWithMalformedJsonResponseEntersErrorState) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, "{not json at all");

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    EXPECT_FALSE(connection.Snapshot().has_config);

    connection.Stop();
}

// The periodic liveness probe's own response (getCurrentActivity) can be
// malformed independently of the config fetch - it must be treated the
// same as any other failed liveness probe (drop the connection, retry),
// not crash or silently keep a stale current_activity_id forever.
TEST_F(HarmonyConnectionTest, MalformedCurrentActivityResponseDropsTheConnection) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    // Consumed by ConnectAndFetchConfig()'s own best-effort fetch - a
    // well-formed response, so has_config/current_activity_id come up
    // clean before the malformed one below is ever reached.
    PushResponse(script, CurrentActivityResponseBody("-1"));
    // Consumed by the first periodic liveness probe after connecting -
    // missing the "result" field FetchCurrentActivity() actually reads.
    PushResponse(script, R"({"data":{"result_typo":"-1"}})");

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff, /*liveness_interval=*/std::chrono::milliseconds(30));
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    // The next liveness probe (30ms later) gets the malformed response
    // above - FetchCurrentActivity() must fail cleanly (not crash, not
    // silently keep the stale current_activity_id forever), and the loop
    // must notice and re-enter its retry cycle rather than staying
    // kConnected on a connection that can't be trusted.
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));

    connection.Stop();
}

// A present-but-wrong-typed "result" (here, an object instead of a
// string/number) must be treated the same as a missing one - not
// silently coerced to "" and accepted as an activity-id change.
TEST_F(HarmonyConnectionTest, WrongTypedCurrentActivityResultDropsTheConnection) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));
    // Consumed by the first periodic liveness probe - "result" is present
    // but an object, not a string/number.
    PushResponse(script, R"({"data":{"result":{}}})");

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff, /*liveness_interval=*/std::chrono::milliseconds(30));
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);
    EXPECT_EQ(connection.Snapshot().current_activity_id, "-1");

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));
    // The last-known-good value must survive the failed probe, not get
    // silently overwritten with "".
    EXPECT_EQ(connection.Snapshot().current_activity_id, "-1");

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, TriggerReconnectPicksUpANewlySavedAddressImmediately) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // Deliberately not configured yet - the unconfigured path's own 5s
    // recheck interval is fixed, not injectable (see harmony_connection.h),
    // so this test relies on TriggerReconnect() to short-circuit that
    // wait rather than waiting one out.

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->responses.push_back(kConfigSuccessBody);
    }

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kDisconnected; }));
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }, /*max_attempts=*/500));

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, ConnectFetchesCurrentActivityAndPublishesItsChange) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("123"));

    std::atomic<int> activity_events{0};
    std::string last_activity_id;
    auto activity_sub = bus.Subscribe<homedeck::HarmonyCurrentActivityChangedEvent>(
        [&](const homedeck::HarmonyCurrentActivityChangedEvent& event) {
            last_activity_id = event.activity_id;
            activity_events++;
        });

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return !connection.Snapshot().current_activity_id.empty(); }));
    EXPECT_EQ(connection.Snapshot().current_activity_id, "123");
    ASSERT_TRUE(WaitFor([&] { return activity_events.load() > 0; }));
    EXPECT_EQ(last_activity_id, "123");

    connection.Stop();
}

// See ConnectAndFetchConfig()'s own comment: a transport failure on its
// best-effort current-activity probe (here, no response queued at all -
// ReceiveText() returns std::nullopt) must fail the whole connect
// attempt, not report a successful kConnected while the socket the
// config fetch itself just opened is already known dead. has_config/
// devices still reflect the config fetch that did succeed (the "keep
// showing last-known state through a transient drop" policy -
// HarmonyConnectionSnapshot::has_config's own comment), but the
// connection itself must never be reported as kConnected.
TEST_F(HarmonyConnectionTest, TransportFailureOnTheBestEffortActivityProbeFailsTheConnectAttempt) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    // Deliberately no CurrentActivityResponseBody queued.

    std::atomic<int> connected_events{0};
    auto state_sub = bus.Subscribe<homedeck::HarmonyConnectionStateChangedEvent>(
        [&connected_events](const homedeck::HarmonyConnectionStateChangedEvent& event) {
            if (event.state == homedeck::HarmonyConnectionState::kConnected) connected_events++;
        });

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_FALSE(connection.Snapshot().devices.empty());
    // Several retry cycles' worth of time (kFastBackoff=30ms each) - long
    // enough that a regression reporting kConnected here would already
    // have shown up.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(connected_events.load(), 0);
    EXPECT_NE(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, StartActivitySendsTheStartCommandAndRefreshesCurrentActivity) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));  // initial current activity: off

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "-1"; }));

    // Queued before StartActivity() so it's ready the instant the
    // connection loop's own immediate follow-up FetchCurrentActivity()
    // call reads it.
    PushResponse(script, CurrentActivityResponseBody("123"));
    connection.StartActivity("123");

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "123"; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        auto start_command = std::find_if(script->sent_texts.begin(), script->sent_texts.end(), [](const std::string& text) {
            return text.find("startactivity") != std::string::npos;
        });
        ASSERT_NE(start_command, script->sent_texts.end());
        EXPECT_NE(start_command->find(R"("activityId":"123")"), std::string::npos);
    }

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, PeriodicLivenessProbePicksUpAHubSideActivityChange) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    // A fast liveness interval (unlike the 30s production default) so the
    // periodic probe path is exercisable without waiting out the full
    // 30s - the same injectable-for-tests shape as
    // initial_backoff/max_backoff.
    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff, /*liveness_interval=*/std::chrono::milliseconds(30));
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "-1"; }));

    // Simulates the hub switching activity on its own (e.g. from its
    // physical remote) - not something StartActivity() triggered here,
    // only the next periodic probe should pick it up.
    PushResponse(script, CurrentActivityResponseBody("123"));

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "123"; }));

    connection.Stop();
}

// A stray already-buffered message (e.g. a reply to an earlier
// SendHoldAction() call, which has no matching receive of its own - see
// DrainStaleMessages()'s own comment) must not get wrongly parsed as the
// liveness probe's own getCurrentActivity reply - DrainStaleMessages()
// must discard it first, leaving the real response for the real receive.
TEST_F(HarmonyConnectionTest, StaleBufferedMessageIsDrainedBeforeTheLivenessProbesOwnReceive) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff, /*liveness_interval=*/std::chrono::milliseconds(30));
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "-1"; }));

    // A stray reply to some earlier holdAction, sitting in the transport's
    // own receive buffer - not this probe's own response, which is pushed
    // separately below (the real backends never confuse the two, since
    // each one's own 0ms poll only sees what's already buffered - see
    // WsScript::stale_responses's own comment).
    PushStaleResponse(script, R"({"data":{"result":"stray-not-a-real-activity-id"}})");
    PushResponse(script, CurrentActivityResponseBody("123"));

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "123"; }));

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, DrainsStaleMessagesBeforeSendingTheInitialConfigRequest) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    connection.Stop();

    // Proves ordering, not just that a drain happens somewhere -
    // WsScript::stale_responses can't model a real transport's shared
    // receive buffer (a real socket doesn't know which call is
    // "supposed" to get which message the way two separate fake queues
    // would), so this checks the one thing that actually matters
    // instead: that ConnectAndFetchConfig()'s own 0ms drain poll runs
    // before the config request itself is sent.
    std::lock_guard<std::mutex> lock(script->mutex);
    auto first_receive0 = std::find(script->call_log.begin(), script->call_log.end(), "receive0");
    auto config_send = std::find_if(script->call_log.begin(), script->call_log.end(),
                                     [](const std::string& entry) { return entry.find("engine?config") != std::string::npos; });
    ASSERT_NE(first_receive0, script->call_log.end());
    ASSERT_NE(config_send, script->call_log.end());
    EXPECT_LT(first_receive0 - script->call_log.begin(), config_send - script->call_log.begin());
}

TEST_F(HarmonyConnectionTest, PressDeviceCommandSendsAPressWithTheGivenAction) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    const std::string action = R"({"command":"VolumeUp","type":"IRCommand","deviceId":"1"})";
    size_t sent_before;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        sent_before = script->sent_texts.size();
    }
    connection.PressDeviceCommand(action);

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->sent_texts.size() >= sent_before + 1;
    }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        const std::string& sent = script->sent_texts[sent_before];
        EXPECT_NE(sent.find("holdAction"), std::string::npos);
        EXPECT_NE(sent.find(R"("status":"press")"), std::string::npos);
        // action is embedded as a JSON string value, so its own quotes
        // come back escaped (\") in the serialized payload.
        EXPECT_NE(sent.find(R"(\"command\":\"VolumeUp\")"), std::string::npos);
    }

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, HoldDeviceCommandSendsAHoldWithTheGivenAction) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    const std::string action = R"({"command":"VolumeUp","type":"IRCommand","deviceId":"1"})";
    size_t sent_before;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        sent_before = script->sent_texts.size();
    }
    connection.HoldDeviceCommand(action);

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->sent_texts.size() >= sent_before + 1;
    }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        const std::string& sent = script->sent_texts[sent_before];
        EXPECT_NE(sent.find("holdAction"), std::string::npos);
        EXPECT_NE(sent.find(R"("status":"hold")"), std::string::npos);
    }

    connection.Stop();
}

TEST_F(HarmonyConnectionTest, ReleaseDeviceCommandSendsAReleaseWithTheGivenAction) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    const std::string action = R"({"command":"VolumeUp","type":"IRCommand","deviceId":"1"})";
    size_t sent_before;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        sent_before = script->sent_texts.size();
    }
    connection.ReleaseDeviceCommand(action);

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->sent_texts.size() >= sent_before + 1;
    }));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        const std::string& sent = script->sent_texts[sent_before];
        EXPECT_NE(sent.find("holdAction"), std::string::npos);
        EXPECT_NE(sent.find(R"("status":"release")"), std::string::npos);
    }

    connection.Stop();
}

// A tap is PressDeviceCommand() immediately followed by
// ReleaseDeviceCommand() (see DevicesScreen's own comment) - this
// confirms the two calls send exactly two messages, in that order, and
// - like a single device command always has - never trigger the
// startactivity-only current-activity refresh.
TEST_F(HarmonyConnectionTest, PressThenReleaseSendsExactlyThoseTwoMessagesInOrderWithNoExtraFetch) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    const std::string action = R"({"command":"VolumeUp","type":"IRCommand","deviceId":"1"})";
    size_t sent_before;
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        sent_before = script->sent_texts.size();
    }
    connection.PressDeviceCommand(action);
    connection.ReleaseDeviceCommand(action);

    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return script->sent_texts.size() >= sent_before + 2;
    }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_NE(script->sent_texts[sent_before].find(R"("status":"press")"), std::string::npos);
        EXPECT_NE(script->sent_texts[sent_before + 1].find(R"("status":"release")"), std::string::npos);
    }

    // A brief settle wait: if the extra fetch were incorrectly sent, it
    // would show up almost immediately, not eventually - unlike the
    // WaitFor() above, this is deliberately checking that something
    // *doesn't* happen.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->sent_texts.size(), sent_before + 2);
    }

    connection.Stop();
}

// A command send failing (e.g. the connection dropped between the last
// liveness probe and this send) must not be silently swallowed - the
// connection loop should notice and re-enter its retry cycle rather than
// staying kConnected while commands quietly go nowhere.
TEST_F(HarmonyConnectionTest, FailedCommandSendDropsTheConnectionAndReconnects) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->send_should_succeed = false;
    }
    connection.PressDeviceCommand(R"({"command":"VolumeUp","type":"IRCommand","deviceId":"1"})");

    // The failed send drops the connection; the reconnect attempt's own
    // config-fetch send fails too (send_should_succeed is still false),
    // so the loop lands in kError rather than silently staying
    // kConnected.
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));

    connection.Stop();
}

// A batch of queued commands must stop at the first failed send rather
// than attempting every remaining entry against a connection that just
// proved it's dead - the second command here should never be attempted.
TEST_F(HarmonyConnectionTest, FailedSendStopsTheBatchWithoutAttemptingLaterCommands) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->send_should_succeed = false;
    }
    // Both are enqueued before the connection loop wakes to drain them -
    // FakeWebSocketClient::SendText() still records a "failed" send into
    // sent_texts (it only fails the return value), so VolumeDown ever
    // appearing would mean the second command was attempted anyway,
    // despite the first one already having failed.
    connection.PressDeviceCommand(R"({"command":"VolumeUp"})");
    connection.PressDeviceCommand(R"({"command":"VolumeDown"})");

    // The failed send drops the connection; the reconnect attempt's own
    // config-fetch send fails too (send_should_succeed is still false),
    // so the loop lands in kError, the same shape
    // FailedCommandSendDropsTheConnectionAndReconnects above already
    // covers - this test's own focus is VolumeDown below, not this part.
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kError; }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        bool volume_up_attempted = false;
        for (const std::string& sent : script->sent_texts) {
            EXPECT_EQ(sent.find("VolumeDown"), std::string::npos) << "second command should never have been attempted";
            if (sent.find("VolumeUp") != std::string::npos) volume_up_attempted = true;
        }
        EXPECT_TRUE(volume_up_attempted) << "first command should have been attempted before failing";
    }

    connection.Stop();
}

// Enqueueing while disconnected (hub_host unset, so the connection loop
// never wakes on a pending command) must not accumulate unbounded - the
// documented cap is 20 (HarmonyConnection::kMaxPendingCommands), and the
// oldest entries are dropped first so the most recent intent survives.
TEST_F(HarmonyConnectionTest, EnqueueingPastTheCapDropsTheOldestEntriesFirst) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // hub_host deliberately not set yet - the connection loop stays in its
    // unconfigured wait, which never wakes early on a queued command (see
    // ConnectionLoop()'s watch_commands=false there), so 25 rapid enqueues
    // below are guaranteed to all land before any get sent.

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    // Waits for the connection loop's own thread to have actually reached
    // its unconfigured wait, rather than assuming it gets there before the
    // 25 enqueues below run - Start() only requests the background thread
    // start, with no guarantee about when it actually begins executing.
    // Without this wait, a slow-to-schedule thread (seen in CI under load)
    // could still be sitting on a stale, pre-Start() TriggerReconnect()-style
    // wake_requested_ once it finally connects, tripping an extra,
    // unnecessary reconnect cycle - kDisconnected is set synchronously
    // before the connection loop's first Sleep() call, so waiting for it
    // here closes that window.
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kDisconnected; }));

    constexpr int kEnqueued = 25;
    constexpr int kCap = 20;
    for (int i = 0; i < kEnqueued; ++i) {
        connection.PressDeviceCommand(R"({"idx":")" + std::to_string(i) + R"("})");
    }

    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        // The connect itself sends 2 messages (config fetch, current-
        // activity fetch) before the capped 20 queued commands drain.
        return script->sent_texts.size() >= 2 + kCap;
    }));

    // A brief settle wait: if more than the cap were sent, it would show
    // up almost immediately.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_EQ(script->sent_texts.size(), 2u + kCap);
        for (const std::string& sent : script->sent_texts) {
            for (int i = 0; i < kEnqueued - kCap; ++i) {
                EXPECT_EQ(sent.find(R"("idx":")" + std::to_string(i) + R"(")"), std::string::npos)
                    << "dropped index " << i << " should never have been sent";
            }
        }
    }

    connection.Stop();
}

// Sleep()'s wait_for predicate checks stop_requested() before checking
// pending_commands_, so a command still queued when Stop() runs is never
// reached by SendPendingCommands() - this must still surface as a
// dropped command, the same as every other loss path in this file, not
// disappear silently just because it lost the race against shutdown.
TEST_F(HarmonyConnectionTest, CommandStillQueuedWhenStoppedPublishesADroppedEvent) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // hub_host deliberately not set - see EnqueueingPastTheCapDropsTheOldestEntriesFirst's
    // own comment: the connection loop stays in its unconfigured wait,
    // which never watches pending_commands_, so the enqueued command
    // below is guaranteed to still be queued, untouched, when Stop() runs.

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kDisconnected; }));

    std::atomic<int> dropped_events{0};
    auto dropped_sub =
        bus.Subscribe<homedeck::HarmonyCommandDroppedEvent>([&](const homedeck::HarmonyCommandDroppedEvent&) { dropped_events++; });

    connection.PressDeviceCommand(R"({"command":"never-sent"})");
    connection.Stop();

    EXPECT_EQ(dropped_events.load(), 1);
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        EXPECT_TRUE(script->sent_texts.empty());
    }
}

// A command still queued from before a connectivity gap no longer
// reflects what the user actually wants sent once too much time has
// passed - it must be dropped, not fired stale on reconnect.
TEST_F(HarmonyConnectionTest, StaleQueuedCommandsAreDroppedOnReconnect) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    // hub_host deliberately not set yet - see the cap test above.

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    constexpr std::chrono::milliseconds kFastMaxAge{50};
    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff, std::chrono::seconds(30), kFastMaxAge);
    connection.Start();

    // ActivitiesScreen's dropped_sub_ (src/ui/screens/activities_screen.h)
    // is the real subscriber - this just verifies the event this class
    // itself is responsible for actually fires, not that screen's own UI
    // reaction to it (LVGL-dependent code isn't built in this test
    // harness - see tests/README.md).
    std::atomic<int> dropped_events{0};
    auto dropped_sub =
        bus.Subscribe<homedeck::HarmonyCommandDroppedEvent>([&](const homedeck::HarmonyCommandDroppedEvent&) { dropped_events++; });

    connection.PressDeviceCommand(R"({"command":"stale"})");
    std::this_thread::sleep_for(kFastMaxAge * 3);  // let it go stale

    connection.PressDeviceCommand(R"({"command":"fresh"})");
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));
    connection.TriggerReconnect();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_TRUE(WaitFor([&] {
        std::lock_guard<std::mutex> lock(script->mutex);
        return std::any_of(script->sent_texts.begin(), script->sent_texts.end(),
                            [](const std::string& s) { return s.find("fresh") != std::string::npos; });
    }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        for (const std::string& sent : script->sent_texts) {
            EXPECT_EQ(sent.find("stale"), std::string::npos) << "stale command should have been dropped, not sent";
        }
    }
    EXPECT_EQ(dropped_events.load(), 1) << "exactly one drop event for the one stale entry in this batch";

    connection.Stop();
}

// A send failing partway through a batch (the WebSocket connection
// itself dropped) must publish the same HarmonyCommandDroppedEvent the
// age-based staleness path does - otherwise a long-press-repeat sequence
// whose connection dies mid-batch loses its command silently, with no
// signal telling ActivitiesScreen/DevicesScreen it never went through.
TEST_F(HarmonyConnectionTest, MidBatchSendFailurePublishesADroppedEvent) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    std::atomic<int> dropped_events{0};
    auto dropped_sub =
        bus.Subscribe<homedeck::HarmonyCommandDroppedEvent>([&](const homedeck::HarmonyCommandDroppedEvent&) { dropped_events++; });

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->send_should_succeed = false;
    }
    connection.PressDeviceCommand(R"({"command":"doomed"})");

    ASSERT_TRUE(WaitFor([&] { return dropped_events.load() > 0; }));
    EXPECT_EQ(dropped_events.load(), 1) << "exactly one drop event for the one entry in this failed batch";

    connection.Stop();
}

// SendPendingCommands()'s own stop.stop_requested() check (harmony_connection.cpp)
// used to return false without publishing HarmonyCommandDroppedEvent, silently
// dropping every command still left in that batch - unlike the stale-age and
// send-failure drops right next to it. Uses WsScript::block_next_send to pin the
// connection loop's own thread inside the first command's SendText() call, so a
// Stop() issued from a second thread is guaranteed to have set the stop token
// before the loop moves on to check the second queued command.
TEST_F(HarmonyConnectionTest, StopDuringAnInFlightBatchSendPublishesADroppedEvent) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    homedeck::EventBus bus;
    FakeHttpClient http_client;
    http_client.SetResponse(homedeck::HttpClientResponse{true, 200, kHandshakeSuccessBody});

    auto script = std::make_shared<WsScript>();
    PushResponse(script, kConfigSuccessBody);
    PushResponse(script, CurrentActivityResponseBody("-1"));

    homedeck::HarmonyConnection connection(
        http_client, [script] { return std::make_unique<FakeWebSocketClient>(script); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));

    std::atomic<int> dropped_events{0};
    auto dropped_sub =
        bus.Subscribe<homedeck::HarmonyCommandDroppedEvent>([&](const homedeck::HarmonyCommandDroppedEvent&) { dropped_events++; });

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->block_next_send = true;
    }
    connection.PressDeviceCommand(R"({"idx":"first"})");
    connection.PressDeviceCommand(R"({"idx":"second"})");

    // Waits for the first command's SendText() to actually be blocked
    // mid-call, not just enqueued - only then is it safe to stop the
    // connection loop's own thread without racing whether it even started
    // sending yet.
    {
        std::unique_lock<std::mutex> lock(script->mutex);
        ASSERT_TRUE(script->send_cv.wait_for(lock, std::chrono::seconds(3), [&] { return script->send_blocked; }));
    }

    // Stop() blocks (via Task's own destructor) until the connection loop's
    // thread actually returns, which can't happen until the blocked
    // SendText() above is released - so it has to run on its own thread.
    // request_stop() itself is a single fast atomic-flag set with no I/O in
    // between it and Stop() being called, so by the time this thread is
    // confirmed running, the stop token has had every practical
    // opportunity to already be set before the release below lets the
    // first command's SendText() return and the loop re-checks it for the
    // second command.
    std::atomic<bool> stopper_running{false};
    std::thread stopper([&] {
        stopper_running = true;
        connection.Stop();
    });
    ASSERT_TRUE(WaitFor([&] { return stopper_running.load(); }));

    {
        std::lock_guard<std::mutex> lock(script->mutex);
        script->release_blocked_send = true;
    }
    script->send_cv.notify_all();
    stopper.join();

    EXPECT_EQ(dropped_events.load(), 1) << "one drop event for the batch, not one per dropped entry";
    {
        std::lock_guard<std::mutex> lock(script->mutex);
        // config fetch + current-activity fetch + the first command - the
        // second command must never reach SendText() at all.
        EXPECT_EQ(script->sent_texts.size(), 3u);
        for (const std::string& sent : script->sent_texts) {
            EXPECT_EQ(sent.find(R"("idx":"second")"), std::string::npos) << "the second command should never have been sent";
        }
    }
}

// Real client/server round trip against real HostHttpClient/
// HostWebSocketClient backends and RunFakeHarmonyHub's own raw-socket
// stand-in hub (this file's own top comment) - the same "test for real,
// not mocked" precedent http_client_test.cpp/websocket_client_test.cpp
// already set for the two backends individually, extended here to prove
// HarmonyConnection's actual connect pipeline (HTTP handshake, WS
// upgrade, config fetch, current-activity probe) round-trips correctly
// over genuine sockets end to end - every other test in this file only
// ever proves this against FakeHttpClient/FakeWebSocketClient.
TEST_F(HarmonyConnectionTest, RealBackendConnectsHandshakesAndFetchesConfigOverActualSocketsAndFraming) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    int listen_fd = ListenOnFakeHubPort();
    ASSERT_GE(listen_fd, 0);
    std::jthread hub_thread([listen_fd] { RunFakeHarmonyHub(listen_fd); });

    homedeck::EventBus bus;
    homedeck::HostHttpClient http_client;

    homedeck::HarmonyConnection connection(
        http_client, [] { return std::make_unique<homedeck::HostWebSocketClient>(); }, storage, bus, kFastBackoff,
        kFastBackoff);
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().has_config; }));
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().state == homedeck::HarmonyConnectionState::kConnected; }));

    homedeck::HarmonyConnectionSnapshot snapshot = connection.Snapshot();
    ASSERT_EQ(snapshot.devices.size(), 1u);
    EXPECT_EQ(snapshot.devices[0].id, "1");
    EXPECT_EQ(snapshot.devices[0].label, "TV");
    ASSERT_EQ(snapshot.activities.size(), 2u);
    EXPECT_EQ(snapshot.activities[1].label, "Watch TV");
    EXPECT_EQ(snapshot.current_activity_id, "-1");

    connection.Stop();
    hub_thread.join();
    close(listen_fd);
}

// See RunFakeHarmonyHubWithStaleFrame()'s own comment - this is the
// timing-sensitive real-socket scenario the test above doesn't cover,
// since its own fake hub only ever sends exactly the reply each request
// expects, in order.
TEST_F(HarmonyConnectionTest, RealBackendDrainsAStaleFrameBeforeTheNextLivenessProbesOwnReceive) {
    homedeck::HostSettingsStore settings_store(root_dir_);
    homedeck::HostCacheStore cache_store(root_dir_);
    homedeck::HostSecretStore secret_store(root_dir_);
    homedeck::Storage storage(settings_store, cache_store, secret_store);
    ASSERT_TRUE(storage.SetSetting("harmony", "hub_host", 1, "127.0.0.1"));

    int listen_fd = ListenOnFakeHubPort();
    ASSERT_GE(listen_fd, 0);
    std::jthread hub_thread([listen_fd] { RunFakeHarmonyHubWithStaleFrame(listen_fd); });

    homedeck::EventBus bus;
    homedeck::HostHttpClient http_client;

    // A fast liveness interval (unlike the 30s production default) so
    // the periodic-probe cycle that reads past the stray frame runs
    // well within this test's own bounded waits - same injectable-for-
    // tests shape the fake-backend tests already use.
    homedeck::HarmonyConnection connection(
        http_client, [] { return std::make_unique<homedeck::HostWebSocketClient>(); }, storage, bus, kFastBackoff,
        kFastBackoff, /*liveness_interval=*/std::chrono::milliseconds(50));
    connection.Start();

    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "-1"; }));

    // If DrainStaleMessages() failed to actually drain the stray frame
    // against the real socket backend, the next liveness probe's own
    // ReceiveText() would read the stray frame instead of the real
    // reply below, and current_activity_id would never reach "123".
    ASSERT_TRUE(WaitFor([&] { return connection.Snapshot().current_activity_id == "123"; }));
    EXPECT_EQ(connection.Snapshot().state, homedeck::HarmonyConnectionState::kConnected);

    connection.Stop();
    hub_thread.join();
    close(listen_fd);
}
