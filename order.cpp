//
// Created by silfo on 2-8-2026.
//
#include <nlohmann/json.hpp>
#include <iostream>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <vector>
#include "websocket.h"
#include "order.h"
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <curl/curl.h>
#include "logger.h"

namespace {
// optional ui log callback (used by the gui, see setOrderLogCallback)
OrderLogCallback g_orderLogCb;

// prices need 2 decimals for alpaca
std::string formatPrice(double price) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << price;
    return oss.str();
}

void logOrder(const std::string& msg, bool isError) {
    if (isError) logError(msg);
    if (g_orderLogCb) {
        g_orderLogCb(msg, isError);
    } else if (isError) {
        std::cerr << msg << "\n";
    } else {
        std::cout << msg << "\n";
    }
}

std::string summarizeError(long httpCode, const std::string& responseBody);
std::string makeClientOrderId();

// shared post logic for alpaca
void postOrder(const nlohmann::json& body) {
    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");

    if (!key || !secret) {
        logError("ALPACA_API_KEY / ALPACA_API_SECRET not set, order cancelled");
        return;
    }

    std::string bodyStr = body.dump();

    // RAII wrappers around the libcurl C handles: the custom deleters make sure
    // the handles are always freed, even if something in between throws. This
    // replaces the old raw CURL*/curl_slist* + manual cleanup calls.
    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    using SlistHandle = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

    CurlHandle curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return;

    curl_slist* slist = nullptr;
    slist = curl_slist_append(slist, ("APCA-API-KEY-ID: " + std::string(key)).c_str());
    slist = curl_slist_append(slist, ("APCA-API-SECRET-KEY: " + std::string(secret)).c_str());
    slist = curl_slist_append(slist, "Content-Type: application/json");
    SlistHandle headers(slist, curl_slist_free_all);

    // capture the response body so we can log errors
    std::string responseBuffer;
    auto writeCallback = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* buf = static_cast<std::string*>(userdata);
        buf->append(ptr, size * nmemb);
        return size * nmemb;
    };

    curl_easy_setopt(curl.get(), CURLOPT_URL, "https://paper-api.alpaca.markets/v2/orders");
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &responseBuffer);

    CURLcode res = curl_easy_perform(curl.get());
    long httpCode = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &httpCode);

    if (res != CURLE_OK) {
        logOrder("Order error (curl): " + std::string(curl_easy_strerror(res)), true);
    } else if (httpCode >= 400) {
        logOrder(summarizeError(httpCode, responseBuffer), true);
    } else {
        logOrder("Order placed: " + responseBuffer, false);
    }
}

std::string summarizeError(long httpCode, const std::string& responseBody) {
    try {
        auto parsed = nlohmann::json::parse(responseBody);
        int code = parsed.value("code", 0);
        std::string msg = parsed.value("message", responseBody);
        return "Order error (HTTP " + std::to_string(httpCode)
               + ", Alpaca code " + std::to_string(code) + "): " + msg;
    } catch (...) {
        return "Order error (HTTP " + std::to_string(httpCode) + "): " + responseBody;
    }
}

std::string makeClientOrderId() {
    static std::atomic<uint64_t> counter{0};
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return "flow++-" + std::to_string(ms) + "-" + std::to_string(counter.fetch_add(1));
}
} // namespace

void setOrderLogCallback(OrderLogCallback cb) {
    g_orderLogCb = std::move(cb);
}

void sendOrder(const std::string& symbol, const std::string& side, const std::string& qty) {
    nlohmann::json body = {
        {"symbol", symbol},
        {"qty", qty},
        {"side", side},
        {"type", "market"},
        {"time_in_force", "day"}
    };
    postOrder(body);
}

void sendBracketOrder(const std::string& symbol,
                       const std::string& side,
                       const std::string& qty,
                       double entryPrice,
                       double takeProfitPrice,
                       double stopLossPrice) {
    if (entryPrice <= 0.0 || takeProfitPrice <= 0.0 || stopLossPrice <= 0.0) {
        logOrder("Order cancelled: invalid (non-positive) prices, no quote yet? "
                 + formatPrice(entryPrice) + " / " + formatPrice(takeProfitPrice)
                 + " / " + formatPrice(stopLossPrice), true);
        return;
    }
    if (side != "buy" && side != "sell") {
        logOrder("Order cancelled: side must be buy or sell, got \"" + side + "\"", true);
        return;
    }
// keep tp/sl at least one tick (0.01) off the entry so the formatted
// prices never end up equal (alpaca rejects brackets with identical tp/sl)
const double MIN_STEP = 0.01;
    if (side == "buy") {
        if (takeProfitPrice < entryPrice + MIN_STEP) takeProfitPrice = entryPrice + MIN_STEP;
        if (stopLossPrice > entryPrice - MIN_STEP) stopLossPrice = entryPrice - MIN_STEP;
    } else {
        if (takeProfitPrice > entryPrice - MIN_STEP) takeProfitPrice = entryPrice - MIN_STEP;
        if (stopLossPrice < entryPrice + MIN_STEP) stopLossPrice = entryPrice + MIN_STEP;
    }

    nlohmann::json body = {
        {"symbol", symbol},
        {"qty", qty},
        {"side", side},
        {"type", "limit"},
        {"limit_price", formatPrice(entryPrice)},
        {"time_in_force", "day"},
        {"order_class", "bracket"},
        {"client_order_id", makeClientOrderId()},
        {"take_profit", {{"limit_price", formatPrice(takeProfitPrice)}}},
        {"stop_loss", {{"stop_price", formatPrice(stopLossPrice)}}}
    };
    postOrder(body);
}
