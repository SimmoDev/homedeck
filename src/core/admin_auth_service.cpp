#include "core/admin_auth_service.h"

#include "core/json_request.h"
#include "third_party/nlohmann/json.hpp"

#include <mbedtls/pkcs5.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>

namespace homedeck {

namespace {

// kModuleId/kPasswordKey live on the class itself now (see
// admin_auth_service.h) - Storage's reserved-key guard needs to
// reference them too. 13 chars - NVS keys are capped at 15
// (NVS_KEY_NAME_MAX_SIZE - 1, see platform/firmware/secret_store.h). A
// longer key silently fails every read/write on firmware
// (ESP_ERR_NVS_INVALID_NAME surfaced as a normal false/nullopt, not a
// crash); HostSecretStore's file-per-key storage has no equivalent
// limit, so this class of bug is invisible on the simulator.
static_assert(std::string_view(AdminAuthService::kPasswordKey).size() <= 15,
              "NVS keys are capped at 15 characters (NVS_KEY_NAME_MAX_SIZE - 1)");
constexpr int kPasswordSchemaVersion = 1;
// PBKDF2-SHA256 iteration counts trade login latency against offline
// brute-force resistance. At 25,000 iterations, login takes ~2 seconds
// on the ESP32-P4 (software SHA256 - see firmware/sdkconfig.defaults's
// CONFIG_MBEDTLS_HARDWARE_SHA=n, off because the hardware-accelerated
// path's fixed per-call GDMA cost dominates over PBKDF2's ~200,000
// individual SHA256 block operations, making it slower overall, not
// faster), a real margin under ESP-IDF's default 5-second FreeRTOS
// task watchdog timeout (CONFIG_ESP_TASK_WDT_TIMEOUT_S). This device's
// threat model (a single local admin account, LAN-only, never
// internet-facing - see
// docs/decisions/ADR-0018-staged-security-hardening.md) is what makes
// that latency/brute-force-resistance trade acceptable; an
// internet-facing account would need a very different number.
constexpr unsigned int kPbkdf2Iterations = 25000;
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kSessionTokenBytes = 32;
constexpr std::chrono::hours kSessionLifetime{24};
constexpr long kSessionLifetimeSeconds = 24 * 60 * 60;
// See security.md's "validate API input" requirement - a floor against
// trivially-guessable passwords, not a full strength policy (no
// composition rules), consistent with this being a single-user LAN
// device rather than an internet-facing account system.
constexpr size_t kMinPasswordLength = 8;
// A defense-in-depth ceiling, not a real strength concern - without it,
// the only bound on a submitted password is the generic
// kMaxHttpRequestBodyBytes cap every endpoint shares, letting a caller
// make PBKDF2 (kPbkdf2Iterations below) hash megabytes of "password" per
// setup/login attempt. Generous enough that no real passphrase is ever
// rejected.
constexpr size_t kMaxPasswordLength = 256;
// PBKDF2's ~2s-per-attempt cost (see kPbkdf2Iterations above) is not by
// itself a brute-force throttle - it bounds an *offline* attacker who
// already has the hash, not an *online* one submitting guesses through
// this same endpoint, which could otherwise still grind at ~0.5
// attempts/sec indefinitely. Global, not per-client (see
// admin_auth_service.h's failed_login_attempts_ comment for why): five
// consecutive failures locks Login() out for a minute regardless of who's
// asking, a real but not user-hostile cost for the occasional mistyped
// password on a single-admin LAN device.
constexpr int kMaxFailedLoginAttempts = 5;
constexpr std::chrono::seconds kLoginLockoutDuration{60};

std::string ToHex(const unsigned char* data, size_t len) {
    static const char kDigits[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result.push_back(kDigits[data[i] >> 4]);
        result.push_back(kDigits[data[i] & 0x0f]);
    }
    return result;
}

std::optional<std::vector<unsigned char>> FromHex(const std::string& hex) {
    if (hex.empty() || hex.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<unsigned char> result(hex.size() / 2);
    for (size_t i = 0; i < result.size(); i++) {
        unsigned int byte;
        if (std::sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) {
            return std::nullopt;
        }
        result[i] = static_cast<unsigned char>(byte);
    }
    return result;
}

// A password/session comparison that short-circuits on the first
// mismatched byte leaks how many leading bytes matched via timing,
// which an attacker can exploit incrementally - so this always walks
// every byte regardless of where the first difference is.
bool ConstantTimeEquals(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

// Same reasoning as the vector overload above, applied to session tokens
// (fixed-length hex strings) - std::string::operator== short-circuits on
// the first mismatched character just like a vector<unsigned char> would.
bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

struct StoredPasswordHash {
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
};

// Parses the "pbkdf2-sha256$<iterations>$<salt_hex>$<hash_hex>" format
// SetInitialPassword() writes (iterations_str is parsed and discarded,
// not used - verification always hashes at the current
// kPbkdf2Iterations constant). Pulled out of Login() so the parsing -
// fast, and the only part of password verification that touches
// Storage - is clearly separated from the expensive hash-and-compare
// step that follows it, which needs none of Login()'s own locked state.
std::optional<StoredPasswordHash> ParseStoredPasswordHash(const std::optional<VersionedValue>& stored) {
    if (!stored.has_value()) {
        return std::nullopt;
    }
    std::istringstream stream(stored->value);
    std::string algorithm, iterations_str, salt_hex, hash_hex;
    std::getline(stream, algorithm, '$');
    std::getline(stream, iterations_str, '$');
    std::getline(stream, salt_hex, '$');
    std::getline(stream, hash_hex, '$');
    if (algorithm != "pbkdf2-sha256" || salt_hex.empty() || hash_hex.empty()) {
        return std::nullopt;
    }
    auto salt = FromHex(salt_hex);
    auto hash = FromHex(hash_hex);
    if (!salt.has_value() || !hash.has_value()) {
        return std::nullopt;
    }
    return StoredPasswordHash{*salt, *hash};
}

// Splits a raw "a=1; b=2" Cookie header (RFC 6265) looking for one
// specific cookie name - the only thing any caller needs so far.
std::optional<std::string> ExtractCookie(const std::string& cookie_header, const std::string& name) {
    size_t pos = 0;
    while (pos < cookie_header.size()) {
        size_t sep = cookie_header.find(';', pos);
        std::string pair = cookie_header.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string key = pair.substr(0, eq);
            size_t start = key.find_first_not_of(' ');  // trim the space left by "; " separators
            if (start != std::string::npos) {
                key = key.substr(start);
            }
            if (key == name) {
                return pair.substr(eq + 1);
            }
        }
        if (sep == std::string::npos) {
            break;
        }
        pos = sep + 1;
    }
    return std::nullopt;
}

}  // namespace

AdminAuthService::AdminAuthService(Storage& storage, TimeSource& time_source)
    : storage_(storage),
      time_source_(time_source),
      entropy_(std::make_unique<mbedtls_entropy_context>()),
      ctr_drbg_(std::make_unique<mbedtls_ctr_drbg_context>()) {
    mbedtls_entropy_init(entropy_.get());
    mbedtls_ctr_drbg_init(ctr_drbg_.get());
    const char* personalization = "homedeck_admin_auth";
    int result = mbedtls_ctr_drbg_seed(ctr_drbg_.get(), mbedtls_entropy_func, entropy_.get(),
                                        reinterpret_cast<const unsigned char*>(personalization),
                                        std::strlen(personalization));
    // Only fails if the platform's entropy source itself is broken -
    // every password hash and session token this service ever issues
    // depends on it being real randomness, so there's no meaningful
    // degraded mode to fall back to.
    if (result != 0) {
        std::abort();
    }
}

AdminAuthService::~AdminAuthService() {
    mbedtls_ctr_drbg_free(ctr_drbg_.get());
    mbedtls_entropy_free(entropy_.get());
}

// Callers of these two must already hold mutex_ - mbedtls_ctr_drbg
// contexts aren't thread-safe, so ctr_drbg_ access must be serialized the
// same way sessions_ is. HashPasswordHex() below is the odd one out: it
// touches no shared state (no ctr_drbg_/entropy_ call), so Login()
// deliberately calls it *without* holding mutex_ - see Login()'s own
// comment for why that matters.
std::vector<unsigned char> AdminAuthService::GenerateSalt() {
    std::vector<unsigned char> salt(kSaltBytes);
    mbedtls_ctr_drbg_random(ctr_drbg_.get(), salt.data(), salt.size());
    return salt;
}

SessionToken AdminAuthService::GenerateSessionToken() {
    unsigned char token[kSessionTokenBytes];
    mbedtls_ctr_drbg_random(ctr_drbg_.get(), token, sizeof(token));
    return ToHex(token, sizeof(token));
}

// No shared state - every input is a parameter, every intermediate is a
// local mbedtls context. Safe to call concurrently from multiple threads
// with no lock of any kind, unlike GenerateSalt()/GenerateSessionToken()
// above.
std::string AdminAuthService::HashPasswordHex(const std::string& password,
                                               const std::vector<unsigned char>& salt) {
    unsigned char output[kHashBytes];
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()),
                                   password.size(), salt.data(), salt.size(), kPbkdf2Iterations, sizeof(output),
                                   output);
    return ToHex(output, sizeof(output));
}

void AdminAuthService::SweepExpiredSessions() {
    // A session nobody ever re-validates after it expires (e.g. a
    // browser tab closed) would otherwise sit in sessions_ forever -
    // ValidateSession() only ever erases the one token it was asked
    // about, never sweeps the rest. Triggered by the same events that
    // grow the map (a new session being issued), so the map can't grow
    // unboundedly between sweeps regardless of whether anything ever
    // calls ValidateSession() again.
    auto now = time_source_.Now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now >= it->second) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

bool AdminAuthService::IsPasswordSet() {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_.GetSecret(kModuleId, kPasswordKey).has_value();
}

size_t AdminAuthService::ActiveSessionCountForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::optional<SessionToken> AdminAuthService::SetInitialPassword(const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_.GetSecret(kModuleId, kPasswordKey).has_value()) {
        return std::nullopt;  // already set - see web-ui.md, this isn't a password-change flow
    }
    std::vector<unsigned char> salt = GenerateSalt();
    std::string hash_hex = HashPasswordHex(password, salt);
    std::string encoded =
        "pbkdf2-sha256$" + std::to_string(kPbkdf2Iterations) + "$" + ToHex(salt.data(), salt.size()) + "$" + hash_hex;
    if (!storage_.SetSecret(kModuleId, kPasswordKey, kPasswordSchemaVersion, encoded)) {
        return std::nullopt;
    }
    SweepExpiredSessions();
    SessionToken token = GenerateSessionToken();
    sessions_[token] = time_source_.Now() + kSessionLifetime;
    return token;
}

std::optional<SessionToken> AdminAuthService::Login(const std::string& password) {
    // Locked out entirely - don't even spend the ~2s PBKDF2 cost on a
    // request that's going to be rejected regardless, and don't let a
    // flood of attempts during the lockout window extend it further. The
    // stored hash is also read here, under the same short-held lock as
    // the lockout check - Storage::GetSecret() is fast (its own internal
    // mutex, not this one), unlike the hash-and-compare step below.
    //
    // The attempt is reserved against failed_login_attempts_ here, before
    // PBKDF2 runs, not after - since the hash runs outside mutex_ (see
    // below), checking-then-incrementing on either side of it would let
    // concurrent callers all pass the lockout check before any of them
    // registered as a failure, allowing more than kMaxFailedLoginAttempts
    // guesses into one window. Reserving up front closes that gap; a
    // correct password un-reserves it again once verified below.
    std::optional<StoredPasswordHash> stored_hash;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (time_source_.Now() < locked_until_) {
            return std::nullopt;
        }
        if (++failed_login_attempts_ >= kMaxFailedLoginAttempts) {
            locked_until_ = time_source_.Now() + kLoginLockoutDuration;
        }
        stored_hash = ParseStoredPasswordHash(storage_.GetSecret(kModuleId, kPasswordKey));
    }

    // HashPasswordHex()'s ~2s PBKDF2 cost touches no shared state (unlike
    // GenerateSalt()/GenerateSessionToken(), it never calls into
    // ctr_drbg_) - computed outside mutex_ so a concurrent Login()/
    // ValidateSession() call from another HTTP worker thread isn't
    // serialized behind it. Holding mutex_ across this used to mean an
    // unauthenticated caller sending a handful of concurrent login
    // attempts before the lockout below engaged could stall every other
    // mutex_-guarded operation - including session validation for every
    // other authenticated endpoint - for the full ~2s each.
    bool authenticated = false;
    if (stored_hash.has_value()) {
        auto computed = FromHex(HashPasswordHex(password, stored_hash->salt));
        authenticated = computed.has_value() && ConstantTimeEquals(*computed, stored_hash->hash);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!authenticated) {
        // Already reserved against failed_login_attempts_ above - nothing
        // more to record on the failure path.
        return std::nullopt;
    }

    // A correct password un-reserves the attempt charged above, including
    // clearing a lockout this exact call may have just triggered - a
    // legitimate login shouldn't lock its own caller out.
    failed_login_attempts_ = 0;
    locked_until_ = std::chrono::system_clock::time_point{};
    SweepExpiredSessions();
    SessionToken token = GenerateSessionToken();
    sessions_[token] = time_source_.Now() + kSessionLifetime;
    return token;
}

bool AdminAuthService::IsLoginLockedOut() {
    std::lock_guard<std::mutex> lock(mutex_);
    return time_source_.Now() < locked_until_;
}

bool AdminAuthService::ValidateSession(const SessionToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Deliberately not sessions_.find(token): an unordered_map lookup
    // resolves via std::string::operator==, which short-circuits on the
    // first mismatched character - the same timing leak ConstantTimeEquals
    // exists to avoid for the password hash above, just for session
    // tokens instead. Sessions are few (single-admin device,
    // SweepExpiredSessions bounds growth), so a linear scan costs nothing
    // meaningful in exchange.
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        if (!ConstantTimeEquals(it->first, token)) {
            continue;
        }
        if (time_source_.Now() >= it->second) {
            sessions_.erase(it);
            return false;
        }
        return true;
    }
    return false;
}

void AdminAuthService::Logout(const SessionToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(token);
}

HttpServer::Handler AdminAuthService::RequireAuth(HttpServer::Handler inner) {
    return [this, inner = std::move(inner)](const HttpRequest& request) -> HttpResponse {
        if (!IsPasswordSet()) {
            return HttpResponse{403, "application/json", R"({"error":"setup_required"})", {}};
        }
        std::optional<std::string> token;
        if (request.cookie_header.has_value()) {
            token = ExtractCookie(*request.cookie_header, "session");
        }
        bool valid = token.has_value() && ValidateSession(*token);
        if (!valid) {
            return HttpResponse{401, "application/json", R"({"error":"unauthenticated"})", {}};
        }
        return inner(request);
    };
}

namespace {

std::string SetCookieHeader(const SessionToken& token, long max_age_seconds) {
    // No Secure attribute - the Web UI is plain HTTP on the LAN (see
    // ADR-0007's accepted single-user-LAN-trust-model risk), so there's
    // no TLS session for Secure to restrict this to. SameSite=Strict
    // since nothing here needs the cookie sent on cross-site requests.
    return "session=" + token + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" +
           std::to_string(max_age_seconds);
}

std::optional<std::string> ReadPasswordField(const std::string& body) {
    auto parsed = TryParseJsonObject(body);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    auto it = parsed->find("password");
    if (it == parsed->end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

}  // namespace

void RegisterAdminAuthRoutes(HttpServer& server, AdminAuthService& auth) {
    server.RegisterHandler(HttpMethod::kGet, "/api/auth/status", [&auth](const HttpRequest& request) {
        bool authenticated = false;
        if (request.cookie_header.has_value()) {
            auto token = ExtractCookie(*request.cookie_header, "session");
            authenticated = token.has_value() && auth.ValidateSession(*token);
        }
        nlohmann::json body = {{"passwordSet", auth.IsPasswordSet()}, {"authenticated", authenticated}};
        return HttpResponse{200, "application/json", body.dump(), {}};
    });

    server.RegisterHandler(HttpMethod::kPost, "/api/auth/setup", [&auth](const HttpRequest& request) {
        if (auth.IsPasswordSet()) {
            return HttpResponse{409, "application/json", R"({"error":"already_set"})", {}};
        }
        auto password = ReadPasswordField(request.body);
        if (!password.has_value()) {
            return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
        }
        if (password->size() < kMinPasswordLength) {
            return HttpResponse{400, "application/json", R"({"error":"password_too_short"})", {}};
        }
        if (password->size() > kMaxPasswordLength) {
            return HttpResponse{400, "application/json", R"({"error":"password_too_long"})", {}};
        }
        auto token = auth.SetInitialPassword(*password);
        if (!token.has_value()) {
            if (auth.IsPasswordSet()) {
                // Lost a race with another request between the
                // IsPasswordSet() check above and SetInitialPassword()
                // itself - see ADR-0007's accepted first-login
                // race-condition risk.
                return HttpResponse{409, "application/json", R"({"error":"already_set"})", {}};
            }
            // SetInitialPassword() can also fail with the password still
            // unset - e.g. the underlying storage write itself failing -
            // which is a different, real error a caller shouldn't be
            // told is "already set."
            return HttpResponse{500, "application/json", R"({"error":"storage_write_failed"})", {}};
        }
        HttpResponse response{200, "application/json", R"({"ok":true})", {}};
        response.extra_headers.push_back({"Set-Cookie", SetCookieHeader(*token, kSessionLifetimeSeconds)});
        return response;
    });

    server.RegisterHandler(HttpMethod::kPost, "/api/auth/login", [&auth](const HttpRequest& request) {
        if (auth.IsLoginLockedOut()) {
            return HttpResponse{429, "application/json", R"({"error":"too_many_attempts"})", {}};
        }
        auto password = ReadPasswordField(request.body);
        if (!password.has_value()) {
            return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
        }
        if (password->size() > kMaxPasswordLength) {
            // Rejected before Login() - no genuine password is this long,
            // so this isn't a real login attempt to count against
            // kMaxFailedLoginAttempts, and it isn't worth PBKDF2's
            // ~2s-per-attempt cost to reject it.
            return HttpResponse{400, "application/json", R"({"error":"password_too_long"})", {}};
        }
        auto token = auth.Login(*password);
        if (!token.has_value()) {
            return HttpResponse{401, "application/json", R"({"error":"invalid_credentials"})", {}};
        }
        HttpResponse response{200, "application/json", R"({"ok":true})", {}};
        response.extra_headers.push_back({"Set-Cookie", SetCookieHeader(*token, kSessionLifetimeSeconds)});
        return response;
    });

    server.RegisterHandler(HttpMethod::kPost, "/api/auth/logout", [&auth](const HttpRequest& request) {
        if (request.cookie_header.has_value()) {
            auto token = ExtractCookie(*request.cookie_header, "session");
            if (token.has_value()) {
                auth.Logout(*token);
            }
        }
        HttpResponse response{200, "application/json", R"({"ok":true})", {}};
        response.extra_headers.push_back({"Set-Cookie", SetCookieHeader("", 0)});
        return response;
    });
}

}  // namespace homedeck
