#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>

// Callback signature: bid, ask, bid_volume, ask_volume
// This keeps the module independent of the OrderBook struct in main.cpp
using QuoteCallback = std::function<void(double bid, double ask,
                                          double bid_volume, double ask_volume)>;

// Status messages for a UI (connecting, authenticated, errors, reconnect, ...)
using StatusCallback = std::function<void(const std::string&)>;

class AlpacaWebSocket {
public:
    AlpacaWebSocket(std::string api_key,
                     std::string api_secret,
                     std::vector<std::string> symbols);

    // Called whenever a new quote arrives
    void setQuoteCallback(QuoteCallback cb);

    // Called on status changes (connecting, errors, ...)
    void setStatusCallback(StatusCallback cb);

    // Blocking call: opens connection, auth, subscribe, and processes events
    void run();

    // Requests a clean shutdown; run() stops shortly after (max a few seconds).
    void stop();

private:
    void notifyStatus(const std::string& msg) const;

    std::string api_key_;
    std::string api_secret_;
    std::vector<std::string> symbols_;
    QuoteCallback callback_;
    StatusCallback status_cb_;
    std::atomic<bool> stop_requested_{false};
    bool connectAndListen();
};
