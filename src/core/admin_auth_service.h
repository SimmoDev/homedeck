#pragma once

#include "core/storage.h"
#include "platform/http_server.h"
#include "platform/time_source.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace homedeck {

using SessionToken = std::string;

// The Web Management UI's admin authentication - see
// docs/architecture/web-ui.md#authentication-mechanism and ADR-0007's
// two decisions (single local admin password with session-based login;
// first-login sets the password, not the SoftAP flow). Portable and
// HTTP-agnostic on purpose - HttpServer::Handler/HttpRequest/HttpResponse
// are the only platform-layer types this touches, the same boundary
// Storage already crosses for its own SettingsStore/CacheStore/
// SecretStore dependencies. The password hash is stored via
// Storage::SetSecret/GetSecret, not SetSetting - see
// docs/decisions/ADR-0010-secret-storage.md#decision-secret-storage-interface.
//
// Unlike most of Core, this is called from HTTP server worker threads,
// not the LVGL UI task - civetweb and esp_http_server can both dispatch
// concurrent requests to different threads. The session table and the
// shared mbedtls RNG context are mutex-guarded because of that, not
// just left single-threaded-safe the way EventBus's non-UI subscribers
// are.
class AdminAuthService {
public:
    // Public so Storage's reserved-key guard (docs/decisions/ADR-0023-settings-backup-api.md)
    // can reference the exact (module_id, key) the admin password hash
    // is stored at, rather than duplicating the literal strings - the
    // guard exists precisely because SecretStore and SettingsStore share
    // the same physical NVS namespace on firmware (see
    // platform/firmware/secret_store.h's own comment).
    static constexpr const char* kModuleId = "core";
    static constexpr const char* kPasswordKey = "admin_pw_hash";

    AdminAuthService(Storage& storage, TimeSource& time_source);
    ~AdminAuthService();

    AdminAuthService(const AdminAuthService&) = delete;
    AdminAuthService& operator=(const AdminAuthService&) = delete;

    bool IsPasswordSet();

    // Only succeeds once - see web-ui.md's first-login-sets-the-password
    // flow; this isn't a general password-change mechanism. Returns a
    // session token on success, the same as Login() would immediately
    // afterward, so first-login doesn't need a second round trip.
    std::optional<SessionToken> SetInitialPassword(const std::string& password);

    std::optional<SessionToken> Login(const std::string& password);
    // True while Login() is throttling after repeated failures - see
    // admin_auth_service.cpp's kMaxFailedLoginAttempts/kLoginLockoutDuration.
    // Exposed so the route layer can respond 429 rather than a plain 401,
    // which would otherwise be indistinguishable from just typing the
    // password wrong.
    bool IsLoginLockedOut();
    bool ValidateSession(const SessionToken& token);
    void Logout(const SessionToken& token);

    // Wraps a handler so it responds 403 (setup required) if no
    // password is set yet, or 401 without a valid session cookie -
    // matching web-ui.md's "until a password is set, the only reachable
    // state is the password-setup screen." Endpoints under /api/auth/*
    // itself must never be wrapped with this, or setup/login could
    // never be reached in the first place.
    HttpServer::Handler RequireAuth(HttpServer::Handler inner);

private:
    SessionToken GenerateSessionToken();
    std::vector<unsigned char> GenerateSalt();
    std::string HashPasswordHex(const std::string& password, const std::vector<unsigned char>& salt);
    // Erases every expired entry from sessions_ - see its own comment
    // for why this is needed at all. Caller must already hold mutex_.
    void SweepExpiredSessions();

    Storage& storage_;
    TimeSource& time_source_;

    std::mutex mutex_;
    // Heap-allocated, not embedded directly - mbedtls_entropy_context in
    // particular is large (a 20-entry source-descriptor array, several
    // hundred bytes), and this class is constructed as a stack-local in
    // app_main(), whose task stack is a small, fixed budget shared with
    // plenty of other locals - unlike the simulator's std::thread, whose
    // default stack is megabytes. Embedding these directly risks a stack
    // overflow on firmware that a host build would never surface.
    std::unique_ptr<mbedtls_entropy_context> entropy_;
    std::unique_ptr<mbedtls_ctr_drbg_context> ctr_drbg_;
    std::unordered_map<SessionToken, std::chrono::system_clock::time_point> sessions_;
    // Login()'s brute-force throttle - see admin_auth_service.cpp's
    // kMaxFailedLoginAttempts/kLoginLockoutDuration. Global, not per-client,
    // since HttpRequest carries no client address (see platform/http_server.h)
    // and this device has exactly one admin account.
    int failed_login_attempts_ = 0;
    std::chrono::system_clock::time_point locked_until_{};
};

// Registers the four endpoints web-ui.md's authentication mechanism
// needs on `server`: GET /api/auth/status (unauthenticated - the
// frontend needs it to decide whether to show setup, login, or the
// authenticated app), POST /api/auth/setup, POST /api/auth/login, and
// POST /api/auth/logout. Must be called before server.Start(), per
// HttpServer's own contract. Shared between simulator/main.cpp and
// firmware/main/homedeck.cpp so the request/response JSON shape only
// exists in one place.
void RegisterAdminAuthRoutes(HttpServer& server, AdminAuthService& auth);

}  // namespace homedeck
