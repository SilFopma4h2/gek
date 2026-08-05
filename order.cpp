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
#include <curl/curl.h>

namespace {
// Optional UI log callback (intended for the GUI; see setOrderLogCallback).
OrderLogCallback g_orderLogCb;

// Helper to format a price on 2 decimals (required by Alpaca).
std::string formatPrice(double price) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << price;
    return oss.str();
}

void logOrder(const std::string& msg, bool isError) {
    if (g_orderLogCb) {
        g_orderLogCb(msg, isError);
    } else if (isError) {
        std::cerr << msg << "\n";
    } else {
        std::cout << msg << "\n";
    }
}

// Shared logic to send an order body to Alpaca.
void postOrder(const nlohmann::json& body) {
    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");

    if (!key || !secret) {
        std::cerr << "ALPACA_API_KEY / ALPACA_API_SECRET not set, order cancelled\n";
        return;
    }

    std::string bodyStr = body.dump();

    CURL* curl = curl_easy_init();
    if (!curl) return;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("APCA-API-KEY-ID: " + std::string(key)).c_str());
    headers = curl_slist_append(headers, ("APCA-API-SECRET-KEY: " + std::string(secret)).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Buffer to capture the response body so errors can be logged.
    std::string responseBuffer;
    auto writeCallback = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* buf = static_cast<std::string*>(userdata);
        buf->append(ptr, size * nmemb);
        return size * nmemb;
    };

    curl_easy_setopt(curl, CURLOPT_URL, "https://paper-api.alpaca.markets/v2/orders");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBuffer);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    if (res != CURLE_OK) {
        logOrder("Order error (curl): " + std::string(curl_easy_strerror(res)), true);
    } else if (httpCode >= 400) {
        logOrder("Order error (HTTP " + std::to_string(httpCode) + "): " + responseBuffer, true);
    } else {
        logOrder("Order placed: " + responseBuffer, false);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
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
    // Make sure TP/SL are at least one tick (0.01) away from the entry in the
    // correct direction, so the formatted prices never coincide and Alpaca
    // does not reject the bracket order due to equal TP/SL.
    const double MIN_STEP = 0.01;
    if (side == "buy") {
        if (takeProfitPrice < entryPrice + MIN_STEP) takeProfitPrice = entryPrice + MIN_STEP;
        if (stopLossPrice > entryPrice - MIN_STEP) stopLossPrice = entryPrice - MIN_STEP;
    } else if (side == "sell") {
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
        {"take_profit", {{"limit_price", formatPrice(takeProfitPrice)}}},
        {"stop_loss", {{"stop_price", formatPrice(stopLossPrice)}}}
    };
    postOrder(body);
}
