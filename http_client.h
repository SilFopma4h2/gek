// http_client.h
//
// Tiny wrapper around libcurl used by the order and position modules.
// Centralises:
//   - curl_global_init/curl_global_cleanup (one-shot, thread-safe)
//   - auth header construction from ALPACA_API_KEY / ALPACA_API_SECRET
//   - the response-body callback
//   - the base URL (paper vs live)
//
// The POST helper is owned by order.cpp because the body shape and the
// status-code handling there is specific. The GET helper lives here so
// position.cpp can share it without re-implementing the same boilerplate.
#pragma once

#include <map>
#include <string>

namespace http {

// base URL for paper vs live trading; paper is the default since we test
// with a paper account.
const char* baseUrl();
void setBaseUrl(const char* url);   // for tests

// thin result of one HTTP call. body is the raw response text (may be
// empty on transport failure); ok is true if libcurl returned CURLE_OK
// and the status code is in [200, 300).
struct Response {
    bool ok = false;
    long status = 0;
    std::string body;
};

// GET request against path (e.g. "/v2/positions"). Returns the parsed
// response; on transport failure the body is empty and status is 0.
Response get(const std::string& path);

// POST request with an empty body (used for DELETE-style operations).
// Returns the parsed response.
Response postEmpty(const std::string& path);

// POST request with a JSON body. Returns the parsed response.
Response postJson(const std::string& path, const std::string& body);

} // namespace http
