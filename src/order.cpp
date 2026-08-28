//
// order.cpp — place orders via the Alpaca REST API. The actual HTTP
// request and auth headers live in http_client.{h,cpp}; this file only
// builds the JSON body and parses the response.
//
#include "order.h"
#include "http_client.h"
#include "logger.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

using json = nlohmann::json;

namespace {

OrderLogCallback g_orderLogCb;
std::mutex g_orderLogMutex;

void logOrder(const std::string& msg, bool isError) {
    if (isError) logError(msg);
    std::lock_guard<std::mutex> lock(g_orderLogMutex);
    if (g_orderLogCb) {
        g_orderLogCb(msg, isError);
    } else if (isError) {
        std::cerr << msg << "\n";
    } else {
        std::cout << msg << "\n";
    }
}

// prices need 2 decimals for alpaca
std::string formatPrice(double price) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << price;
    return oss.str();
}

// concise summary of an Alpaca error response (Alpaca's body is JSON
// {"code":..., "message":...}; fall back to a short snippet if parsing fails)
std::string summarizeError(long httpCode, const std::string& responseBody) {
    if (responseBody.empty()) {
        return "HTTP " + std::to_string(httpCode) + " (empty body)";
    }
    try {
        auto j = json::parse(responseBody);
        std::string code, msg;
        if (j.contains("code") && j["code"].is_number_integer()) {
            code = std::to_string(j["code"].get<int>());
        }
        if (j.contains("message") && j["message"].is_string()) {
            msg = j["message"].get<std::string>();
        }
        if (!code.empty() && !msg.empty()) return "HTTP " + std::to_string(httpCode) + " [" + code + "] " + msg;
        if (!msg.empty()) return "HTTP " + std::to_string(httpCode) + " " + msg;
    } catch (...) {}
    std::string snippet = responseBody.substr(0, std::min<size_t>(responseBody.size(), 200));
    return "HTTP " + std::to_string(httpCode) + " " + snippet;
}

// each order needs a unique client_order_id; we mix a per-process counter
// with a high-resolution clock so two orders started back-to-back can't
// collide.
std::string makeClientOrderId() {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::ostringstream oss;
    oss << "flowpp-" << ns << "-" << counter.fetch_add(1, std::memory_order_relaxed);
    return oss.str();
}

void postOrder(const json& body) {
    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");

    if (!key || !secret) {
        logError("ALPACA_API_KEY / ALPACA_API_SECRET not set, order cancelled");
        return;
    }

    auto resp = http::postJson("/v2/orders", body.dump());
    if (resp.ok) {
        logOrder("Order accepted: " + resp.body, false);
    } else {
        logOrder("Order rejected: " + summarizeError(resp.status, resp.body), true);
    }
}

} // namespace

void setOrderLogCallback(OrderLogCallback cb) {
    std::lock_guard<std::mutex> lock(g_orderLogMutex);
    g_orderLogCb = std::move(cb);
}

void sendOrder(const std::string& symbol, const std::string& side, const std::string& qty) {
    if (std::stod(qty) <= 0.0) {
        logError("sendOrder: qty <= 0, order cancelled");
        return;
    }
    json body = {
        {"symbol", symbol},
        {"qty", qty},
        {"side", side},
        {"type", "market"},
        {"time_in_force", "day"},
        {"client_order_id", makeClientOrderId()}
    };
    postOrder(body);
}

void sendBracketOrder(const std::string& symbol,
                      const std::string& side,
                      const std::string& qty,
                      double entryPrice,
                      double takeProfitPrice,
                      double stopLossPrice) {
    if (std::stod(qty) <= 0.0) {
        logError("sendBracketOrder: qty <= 0, order cancelled");
        return;
    }
    if (entryPrice <= 0.0) {
        logError("sendBracketOrder: entryPrice <= 0, order cancelled");
        return;
    }
    json body = {
        {"symbol", symbol},
        {"qty", qty},
        {"side", side},
        {"type", "limit"},
        {"time_in_force", "day"},
        {"limit_price", formatPrice(entryPrice)},
        {"client_order_id", makeClientOrderId()},
        {"order_class", "bracket"},
        {"take_profit", {{"limit_price", formatPrice(takeProfitPrice)}}},
        {"stop_loss",   {{"stop_price",  formatPrice(stopLossPrice)}}}
    };
    postOrder(body);
}
