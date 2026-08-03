#include "core/admin_auth_service.h"

#include "platform/host/cache_store.h"
#include "platform/host/secret_store.h"
#include "platform/host/settings_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace {

class FakeTimeSource : public homedeck::TimeSource {
public:
    std::chrono::system_clock::time_point Now() const override { return fixed_time; }

    std::chrono::system_clock::time_point fixed_time =
        std::chrono::system_clock::time_point(std::chrono::seconds(1700000000));
};

class AdminAuthServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_dir_ = std::filesystem::path(::testing::TempDir()) /
                    ("homedeck_admin_auth_test_" +
                     std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::filesystem::remove_all(root_dir_);
        settings_store_ = std::make_unique<homedeck::HostSettingsStore>(root_dir_);
        cache_store_ = std::make_unique<homedeck::HostCacheStore>(root_dir_);
        secret_store_ = std::make_unique<homedeck::HostSecretStore>(root_dir_);
        storage_ = std::make_unique<homedeck::Storage>(*settings_store_, *cache_store_, *secret_store_);
    }

    void TearDown() override { std::filesystem::remove_all(root_dir_); }

    std::filesystem::path root_dir_;
    std::unique_ptr<homedeck::HostSettingsStore> settings_store_;
    std::unique_ptr<homedeck::HostCacheStore> cache_store_;
    std::unique_ptr<homedeck::HostSecretStore> secret_store_;
    std::unique_ptr<homedeck::Storage> storage_;
    FakeTimeSource time_source_;
};

}  // namespace

TEST_F(AdminAuthServiceTest, PasswordIsNotSetInitially) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    EXPECT_FALSE(auth.IsPasswordSet());
}

TEST_F(AdminAuthServiceTest, SetInitialPasswordSucceedsOnceAndIssuesAValidSession) {
    homedeck::AdminAuthService auth(*storage_, time_source_);

    auto token = auth.SetInitialPassword("correct horse battery staple");
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(auth.IsPasswordSet());
    EXPECT_TRUE(auth.ValidateSession(*token));
}

TEST_F(AdminAuthServiceTest, SetInitialPasswordFailsOnceAlreadySet) {
    homedeck::AdminAuthService auth(*storage_, time_source_);

    ASSERT_TRUE(auth.SetInitialPassword("first password").has_value());
    EXPECT_FALSE(auth.SetInitialPassword("second password").has_value());
}

TEST_F(AdminAuthServiceTest, LoginWithCorrectPasswordSucceeds) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    ASSERT_TRUE(auth.SetInitialPassword("correct horse battery staple").has_value());

    auto token = auth.Login("correct horse battery staple");
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(auth.ValidateSession(*token));
}

TEST_F(AdminAuthServiceTest, LoginWithWrongPasswordFails) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    ASSERT_TRUE(auth.SetInitialPassword("correct horse battery staple").has_value());

    EXPECT_FALSE(auth.Login("wrong password").has_value());
}

TEST_F(AdminAuthServiceTest, LoginLocksOutAfterRepeatedFailuresThenRecoversOnceItExpires) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    ASSERT_TRUE(auth.SetInitialPassword("correct horse battery staple").has_value());

    // kMaxFailedLoginAttempts (5, admin_auth_service.cpp) consecutive
    // wrong guesses.
    for (int i = 0; i < 5; i++) {
        EXPECT_FALSE(auth.Login("wrong password").has_value());
    }
    EXPECT_TRUE(auth.IsLoginLockedOut());

    // Locked out now - even the correct password is rejected without a
    // real check, not just another wrong-password miss.
    EXPECT_FALSE(auth.Login("correct horse battery staple").has_value());

    time_source_.fixed_time += std::chrono::seconds(61);  // past kLoginLockoutDuration (60s)

    EXPECT_FALSE(auth.IsLoginLockedOut());
    auto token = auth.Login("correct horse battery staple");
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(auth.ValidateSession(*token));
}

TEST_F(AdminAuthServiceTest, SuccessfulLoginResetsTheFailureCounter) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    ASSERT_TRUE(auth.SetInitialPassword("correct horse battery staple").has_value());

    for (int i = 0; i < 4; i++) {  // one under the lockout threshold
        EXPECT_FALSE(auth.Login("wrong password").has_value());
    }
    ASSERT_TRUE(auth.Login("correct horse battery staple").has_value());
    EXPECT_FALSE(auth.IsLoginLockedOut());

    // Counter restarted from zero, not just paused at 4 - four more
    // failures alone must not re-trigger the lockout.
    for (int i = 0; i < 4; i++) {
        EXPECT_FALSE(auth.Login("wrong password").has_value());
    }
    EXPECT_FALSE(auth.IsLoginLockedOut());
}

TEST_F(AdminAuthServiceTest, LoginBeforeAnyPasswordIsSetFails) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    EXPECT_FALSE(auth.Login("anything").has_value());
}

TEST_F(AdminAuthServiceTest, ValidateSessionRejectsAnUnknownToken) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    EXPECT_FALSE(auth.ValidateSession("not-a-real-token"));
}

TEST_F(AdminAuthServiceTest, LogoutInvalidatesTheSession) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    auto token = auth.SetInitialPassword("correct horse battery staple");
    ASSERT_TRUE(token.has_value());

    auth.Logout(*token);

    EXPECT_FALSE(auth.ValidateSession(*token));
}

TEST_F(AdminAuthServiceTest, SessionExpiresAfterItsLifetime) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    auto token = auth.SetInitialPassword("correct horse battery staple");
    ASSERT_TRUE(token.has_value());
    ASSERT_TRUE(auth.ValidateSession(*token));

    time_source_.fixed_time += std::chrono::hours(25);  // past the 24h session lifetime

    EXPECT_FALSE(auth.ValidateSession(*token));
}

TEST_F(AdminAuthServiceTest, LoginSweepsAnExpiredSessionNobodyEverRevalidated) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    auto first_token = auth.SetInitialPassword("correct horse battery staple");
    ASSERT_TRUE(first_token.has_value());
    ASSERT_EQ(auth.ActiveSessionCountForTesting(), 1u);

    time_source_.fixed_time += std::chrono::hours(25);  // past the 24h session lifetime

    // A second login issues a new session without anyone ever having
    // re-validated (and thereby lazily expired) the first one.
    auto second_token = auth.Login("correct horse battery staple");
    ASSERT_TRUE(second_token.has_value());

    // The stale first session must be gone, not just a second one added
    // on top of it - proves SweepExpiredSessions() actually ran on this
    // login, not just that ValidateSession()'s own lazy expiry works
    // (already covered by SessionExpiresAfterItsLifetime above).
    EXPECT_EQ(auth.ActiveSessionCountForTesting(), 1u);
}

TEST_F(AdminAuthServiceTest, RequireAuthReturns403WhenNoPasswordIsSetYet) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    bool inner_called = false;
    auto wrapped = auth.RequireAuth([&inner_called](const homedeck::HttpRequest&) {
        inner_called = true;
        return homedeck::HttpResponse{200, "text/plain", "secret", {}};
    });

    homedeck::HttpResponse response = wrapped(homedeck::HttpRequest{});

    EXPECT_EQ(response.status_code, 403);
    EXPECT_FALSE(inner_called);
}

TEST_F(AdminAuthServiceTest, RequireAuthReturns401WithoutAValidSessionCookie) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    ASSERT_TRUE(auth.SetInitialPassword("correct horse battery staple").has_value());
    bool inner_called = false;
    auto wrapped = auth.RequireAuth([&inner_called](const homedeck::HttpRequest&) {
        inner_called = true;
        return homedeck::HttpResponse{200, "text/plain", "secret", {}};
    });

    homedeck::HttpRequest request;
    request.cookie_header = "session=not-a-real-token";
    homedeck::HttpResponse response = wrapped(request);

    EXPECT_EQ(response.status_code, 401);
    EXPECT_FALSE(inner_called);
}

namespace {
// Decorates a real SecretStore to count Get() calls - a proxy for "how
// many Login() calls actually got past the lockout gate to attempt a
// real password comparison," since Login() only reads the stored hash
// from inside that gate.
class CountingSecretStore : public homedeck::SecretStore {
public:
    explicit CountingSecretStore(homedeck::SecretStore& inner) : inner_(inner) {}
    bool Set(const std::string& ns, const std::string& key, const std::string& value) override {
        return inner_.Set(ns, key, value);
    }
    std::optional<std::string> Get(const std::string& ns, const std::string& key) override {
        get_calls.fetch_add(1);
        return inner_.Get(ns, key);
    }
    bool Erase(const std::string& ns, const std::string& key) override { return inner_.Erase(ns, key); }

    std::atomic<int> get_calls{0};

private:
    homedeck::SecretStore& inner_;
};
}  // namespace

TEST_F(AdminAuthServiceTest, ConcurrentWrongPasswordAttemptsNeverExceedTheLockoutThresholdBeforeLockingOut) {
    // Regression test: the attempt used to be recorded against
    // failed_login_attempts_ only *after* PBKDF2 ran (see Login()'s own
    // comment), so concurrent callers could all pass the "not locked out"
    // check before any of them registered as a failure - letting an
    // unbounded number of concurrent guesses reach the real password
    // comparison within one lockout window, regardless of
    // kMaxFailedLoginAttempts. The attempt is now reserved up front,
    // before hashing, closing that gap.
    CountingSecretStore counting_secret_store(*secret_store_);
    homedeck::Storage storage(*settings_store_, *cache_store_, counting_secret_store);
    homedeck::AdminAuthService auth(storage, time_source_);
    ASSERT_TRUE(auth.SetInitialPassword("correct horse battery staple").has_value());
    counting_secret_store.get_calls = 0;  // reset after SetInitialPassword's own reads

    constexpr int kConcurrentAttempts = 20;  // well over kMaxFailedLoginAttempts (5)
    std::vector<std::thread> attackers;
    for (int i = 0; i < kConcurrentAttempts; i++) {
        attackers.emplace_back([&] { auth.Login("wrong password"); });
    }
    for (auto& t : attackers) t.join();

    EXPECT_TRUE(auth.IsLoginLockedOut());
    // At most kMaxFailedLoginAttempts calls should ever have reached the
    // real password comparison - the rest must have been rejected by the
    // lockout gate before reading the stored hash at all.
    EXPECT_LE(counting_secret_store.get_calls.load(), 5);
}

TEST_F(AdminAuthServiceTest, ConcurrentLoginAndValidateSessionDoNotRaceOrCorruptState) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    auto initial_token = auth.SetInitialPassword("correct horse battery staple");
    ASSERT_TRUE(initial_token.has_value());

    // Real HTTP-worker-thread concurrency is exactly what mutex_ exists to
    // guard against (see admin_auth_service.h's own class comment) - this
    // drives ValidateSession() from several real threads while a separate
    // thread runs real Login() attempts (the expensive PBKDF2 path
    // Login()'s own mutex-scope fix now runs outside the lock for), the
    // same shape of test as ota_routes_test.cpp's
    // ConcurrentUploadIsRejectedRatherThanRacingTheFirst. A session
    // unrelated to any of the concurrent Login() activity must never be
    // corrupted or evicted by it.
    std::atomic<bool> stop{false};
    std::atomic<int> validate_failures{0};

    std::vector<std::thread> validators;
    for (int i = 0; i < 4; i++) {
        validators.emplace_back([&] {
            while (!stop.load()) {
                if (!auth.ValidateSession(*initial_token)) {
                    validate_failures.fetch_add(1);
                }
            }
        });
    }

    std::thread logins([&] {
        for (int i = 0; i < 3; i++) {
            auth.Login("wrong password");
        }
        EXPECT_TRUE(auth.Login("correct horse battery staple").has_value());
    });

    logins.join();
    stop.store(true);
    for (auto& validator : validators) {
        validator.join();
    }

    EXPECT_EQ(validate_failures.load(), 0);
    EXPECT_TRUE(auth.ValidateSession(*initial_token));
}

TEST_F(AdminAuthServiceTest, RequireAuthCallsInnerHandlerWithAValidSessionCookie) {
    homedeck::AdminAuthService auth(*storage_, time_source_);
    auto token = auth.SetInitialPassword("correct horse battery staple");
    ASSERT_TRUE(token.has_value());
    auto wrapped = auth.RequireAuth(
        [](const homedeck::HttpRequest&) { return homedeck::HttpResponse{200, "text/plain", "secret", {}}; });

    homedeck::HttpRequest request;
    // Alongside an unrelated cookie, matching a real browser's Cookie
    // header carrying more than one cookie - proves the "session" name
    // is actually parsed out, not just assumed to be the whole header.
    request.cookie_header = "other=1; session=" + *token;
    homedeck::HttpResponse response = wrapped(request);

    EXPECT_EQ(response.status_code, 200);
    EXPECT_EQ(response.body, "secret");
}
