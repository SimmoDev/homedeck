#include "platform/host/http_client.h"

#include <curl/curl.h>

namespace homedeck {

namespace {

// Bounds how long Task::~Task() can block on an in-flight fetch - see
// core/weather_provider.h's own comment on why an unbounded timeout
// would undercut the chunked-sleep "prompt shutdown" contract that
// exists specifically so a Task built on this doesn't hang on
// destruction.
constexpr long kTimeoutSeconds = 10;

// curl_global_init() is explicitly not thread-safe and must run exactly
// once before any curl handle is used - a function-local static gives
// that via C++11's guaranteed thread-safe one-time initialization,
// regardless of how many HostHttpClient instances get constructed.
void EnsureGlobalInit() {
    static int unused = (curl_global_init(CURL_GLOBAL_DEFAULT), 0);
    (void)unused;
}

size_t WriteToString(char* data, size_t size, size_t count, void* user_data) {
    auto* body = static_cast<std::string*>(user_data);
    // Returning less than size*count tells curl the write failed, which
    // aborts the transfer with CURLE_WRITE_ERROR - PerformCommon() then
    // reports success=false, the same as any other transport failure.
    // See kMaxHttpResponseBodyBytes's own comment for why.
    if (body->size() + size * count > kMaxHttpResponseBodyBytes) {
        return 0;
    }
    body->append(data, size * count);
    return size * count;
}

// Shared by Get()/Post() below - runs whatever request-specific options
// the caller already set on curl (method, body), then performs and
// extracts the common success/status/body shape both callers return.
HttpClientResponse PerformCommon(CURL* curl) {
    std::string body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode result = curl_easy_perform(curl);

    HttpClientResponse response;
    response.success = (result == CURLE_OK);
    response.status_code = 0;
    if (response.success) {
        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        response.status_code = static_cast<int>(status_code);
    }
    response.body = std::move(body);
    return response;
}

}  // namespace

HttpClientResponse HostHttpClient::Get(const std::string& url) {
    EnsureGlobalInit();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        HttpClientResponse response;
        response.success = false;
        response.status_code = 0;
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    HttpClientResponse response = PerformCommon(curl);
    curl_easy_cleanup(curl);
    return response;
}

HttpClientResponse HostHttpClient::Post(const std::string& url, const std::string& json_body,
                                         const std::vector<std::pair<std::string, std::string>>& extra_headers) {
    EnsureGlobalInit();

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        HttpClientResponse response;
        response.success = false;
        response.status_code = 0;
        return response;
    }

    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    for (const auto& [name, value] : extra_headers) {
        headers = curl_slist_append(headers, (name + ": " + value).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json_body.size()));

    HttpClientResponse response = PerformCommon(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

}  // namespace homedeck
