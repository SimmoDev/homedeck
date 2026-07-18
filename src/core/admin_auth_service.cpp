#include "core/admin_auth_service.h"

#include "third_party/nlohmann/json.hpp"

#include <mbedtls/pkcs5.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string_view>

namespace homedeck {

namespace {

constexpr const char* kModuleId = "core";
// 13 chars - NVS keys are capped at 15 (NVS_KEY_NAME_MAX_SIZE - 1, see
// platform/firmware/settings_store.h). A longer key silently fails
// every read/write on firmware (ESP_ERR_NVS_INVALID_NAME surfaced as a
// normal false/nullopt, not a crash); HostSettingsStore's file-per-key
// storage has no equivalent limit, so this class of bug is invisible on
// the simulator.
constexpr const char* kPasswordKey = "admin_pw_hash";
static_assert(std::string_view(kPasswordKey).size() <= 15,
              "NVS keys are capped at 15 characters (NVS_KEY_NAME_MAX_SIZE - 1)");
constexpr int kPasswordSchemaVersion = 1;
// Provisional - PBKDF2-SHA256 iteration counts trade login latency
// against offline brute-force resistance. At this count, ESP-IDF's
// default hardware-accelerated SHA256 takes 30+ seconds on the
// ESP32-P4, long enough to trip the FreeRTOS task watchdog - see
// web-ui.md#status. Hardware SHA acceleration is disabled project-wide
// (firmware/sdkconfig.defaults) rather than lowering this count; real
// timing with it disabled hasn't been measured yet.
constexpr unsigned int kPbkdf2Iterations = 100000;
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

// Callers of these three helpers already hold mutex_ - mbedtls contexts
// aren't thread-safe, so ctr_drbg_ access must be serialized the same
// way sessions_ is.
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

std::string AdminAuthService::HashPasswordHex(const std::string& password,
                                               const std::vector<unsigned char>& salt) {
    unsigned char output[kHashBytes];
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()),
                                   password.size(), salt.data(), salt.size(), kPbkdf2Iterations, sizeof(output),
                                   output);
    return ToHex(output, sizeof(output));
}

bool AdminAuthService::IsPasswordSet() {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_.GetSetting(kModuleId, kPasswordKey).has_value();
}

std::optional<SessionToken> AdminAuthService::SetInitialPassword(const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_.GetSetting(kModuleId, kPasswordKey).has_value()) {
        return std::nullopt;  // already set - see web-ui.md, this isn't a password-change flow
    }
    std::vector<unsigned char> salt = GenerateSalt();
    std::string hash_hex = HashPasswordHex(password, salt);
    std::string encoded =
        "pbkdf2-sha256$" + std::to_string(kPbkdf2Iterations) + "$" + ToHex(salt.data(), salt.size()) + "$" + hash_hex;
    if (!storage_.SetSetting(kModuleId, kPasswordKey, kPasswordSchemaVersion, encoded)) {
        return std::nullopt;
    }
    SessionToken token = GenerateSessionToken();
    sessions_[token] = time_source_.Now() + kSessionLifetime;
    return token;
}

std::optional<SessionToken> AdminAuthService::Login(const std::string& password) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stored = storage_.GetSetting(kModuleId, kPasswordKey);
    if (!stored.has_value()) {
        return std::nullopt;
    }

    // Format: pbkdf2-sha256$<iterations>$<salt_hex>$<hash_hex> - a
    // self-describing format (deliberately not just a bare hash) so a
    // future iteration-count or algorithm change can recognize and
    // migrate old entries instead of invalidating every stored password.
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
    auto expected = FromHex(hash_hex);
    if (!salt.has_value() || !expected.has_value()) {
        return std::nullopt;
    }
    auto computed = FromHex(HashPasswordHex(password, *salt));
    if (!computed.has_value() || !ConstantTimeEquals(*computed, *expected)) {
        return std::nullopt;
    }

    SessionToken token = GenerateSessionToken();
    sessions_[token] = time_source_.Now() + kSessionLifetime;
    return token;
}

bool AdminAuthService::ValidateSession(const SessionToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) {
        return false;
    }
    if (time_source_.Now() >= it->second) {
        sessions_.erase(it);
        return false;
    }
    return true;
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
    nlohmann::json parsed = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
    }
    auto it = parsed.find("password");
    if (it == parsed.end() || !it->is_string()) {
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
        auto password = ReadPasswordField(request.body);
        if (!password.has_value()) {
            return HttpResponse{400, "application/json", R"({"error":"invalid_request"})", {}};
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
