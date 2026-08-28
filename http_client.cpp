// http_client.cpp
#include "http_client.h"
#include "logger.h"

#include <atomic>
#include <curl/curl.h>
#include <mutex>
#include <sstream>

namespace http {

namespace {

// one-time libcurl global init. CURL_GLOBAL_ALL is the documented choice
// for multi-threaded apps; without it, curl_easy_init is not guaranteed
// to be thread-safe on all libcurl versions/builds.
void ensureGlobalInit() {
    static std::atomic<int> initialised{0};
    if (initialised.load(std::memory_order_acquire) == 0) {
        static std::mutex initMutex;
        std::lock_guard<std::mutex> lock(initMutex);
        if (initialised.load(std::memory_order_relaxed) == 0) {
            curl_global_init(CURL_GLOBAL_ALL);
            initialised.store(1, std::memory_order_release);
        }
    }
}

std::atomic<const char*> g_baseUrl{nullptr};

const char* resolveBaseUrl() {
    if (const char* override_ = g_baseUrl.load(std::memory_order_acquire)) {
        return override_;
    }
    return "https://paper-api.alpaca.markets";
}

size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

struct CurlSlist {
    curl_slist* p = nullptr;
    ~CurlSlist() { if (p) curl_slist_free_all(p); }
    void append(const char* s) { p = curl_slist_append(p, s); }
};

struct CurlEasy {
    CURL* h = nullptr;
    ~CurlEasy() { if (h) curl_easy_cleanup(h); }
    operator CURL*() const { return h; }
};

Response perform(CURL* curl) {
    Response r;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "flow++/1.0");

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        r.status = code;
        r.ok = (code >= 200 && code < 300);
    } else {
        std::ostringstream oss;
        oss << "HTTP transport error: " << curl_easy_strerror(rc)
            << " (" << static_cast<int>(rc) << ")";
        logError(oss.str());
    }
    return r;
}

void attachAuthHeaders(CurlSlist& headers) {
    if (const char* key = std::getenv("ALPACA_API_KEY")) {
        std::string h = "APCA-API-KEY-ID: ";
        h += key;
        headers.append(h.c_str());
    }
    if (const char* sec = std::getenv("ALPACA_API_SECRET")) {
        std::string h = "APCA-API-SECRET-KEY: ";
        h += sec;
        headers.append(h.c_str());
    }
    headers.append("Accept: application/json");
}

} // namespace

const char* baseUrl() {
    return resolveBaseUrl();
}

void setBaseUrl(const char* url) {
    g_baseUrl.store(url, std::memory_order_release);
}

Response get(const std::string& path) {
    ensureGlobalInit();
    CurlEasy easy;
    easy.h = curl_easy_init();
    if (!easy.h) {
        logError("curl_easy_init failed");
        return {};
    }
    CurlSlist headers;
    attachAuthHeaders(headers);
    std::string url = resolveBaseUrl();
    url += path;

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.p);
    return perform(easy);
}

Response postEmpty(const std::string& path) {
    ensureGlobalInit();
    CurlEasy easy;
    easy.h = curl_easy_init();
    if (!easy.h) {
        logError("curl_easy_init failed");
        return {};
    }
    CurlSlist headers;
    attachAuthHeaders(headers);
    std::string url = resolveBaseUrl();
    url += path;

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.p);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, "");
    return perform(easy);
}

Response postJson(const std::string& path, const std::string& body) {
    ensureGlobalInit();
    CurlEasy easy;
    easy.h = curl_easy_init();
    if (!easy.h) {
        logError("curl_easy_init failed");
        return {};
    }
    CurlSlist headers;
    attachAuthHeaders(headers);
    headers.append("Content-Type: application/json");
    std::string url = resolveBaseUrl();
    url += path;

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers.p);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)body.size());
    return perform(easy);
}

} // namespace http
