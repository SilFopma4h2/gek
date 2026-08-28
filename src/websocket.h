#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>

// callback: bid, ask, bid_volume, ask_volume
// keeps this module decoupled from the OrderBook struct in main.cpp
using QuoteCallback = std::function<void(double bid, double ask,
                                          double bid_volume, double ask_volume)>;

// status messages for the ui (connecting, auth, errors, reconnect, ...)
using StatusCallback = std::function<void(const std::string&)>;

class AlpacaWebSocket {
public:
    AlpacaWebSocket(std::string api_key,
                     std::string api_secret,
                     std::vector<std::string> symbols);

    // called on every new quote
    void setQuoteCallback(QuoteCallback cb);

    // called on status changes (connecting, errors, ...)
    void setStatusCallback(StatusCallback cb);

    // blocking: connect, auth, subscribe, process events
    void run();

    // asks for a clean shutdown; run() returns shortly after
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
