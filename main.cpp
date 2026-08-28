#include <iostream>
#include <cmath>
#include <deque>
#include <vector>
#include <algorithm>
#include <ctime>
#include "websocket.h"
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <chrono>
#include <csignal>
#include "order.h"
#include "core.h"
#include "experts.h"
#include "logger.h"

// config
const std::string symbol = "SPY";
// decide on a time window (5 min), not on a tick count
const int DECISION_INTERVAL_SECONDS = 5 * 60;

// set to true to actually place orders
const bool orderyes = true;

// tp/sl sized off the average spread in the window
// 2:1 rr, tp further away than sl
const double TP_SPREAD_MULTIPLIER = 3.0;
const double SL_SPREAD_MULTIPLIER = 1.5;

// incoming quotes; capped at 500 (old quote-book history, kept for parity
// with the GUI's g_bookData buffer). The signal logic itself lives in
// DecisionCore.
std::deque<OrderBook> symbolOrderBook;
static std::mutex bookMutex;
static std::condition_variable bookCv;
static bool newDataAvailable = false;

// so we can stop cleanly on ctrl-c etc
static volatile std::sig_atomic_t g_stop = 0;
static void handleSignal(int) {
    g_stop = 1;
}

static std::string nowStr() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

// bracket order (tp/sl) from the majority signal; entry is a limit at
// the current mid so tp/sl anchor to a known price
static void placeOrder(const std::string& signal, const Decision& d) {
    if (!orderyes) {
        std::cout << "orders are disabled, no orders placed\n";
        return;
    }
    if (signal == "BUY") {
        double tp = d.lastMid + TP_SPREAD_MULTIPLIER * d.avgSpread;
        double sl = d.lastMid - SL_SPREAD_MULTIPLIER * d.avgSpread;
        std::cout << "-> BUY limit+bracket: entry=" << d.lastMid
                  << " qty=1, TP=" << tp << " SL=" << sl
                  << " (avgSpread=" << d.avgSpread << ")\n";
        sendBracketOrder(symbol, "buy", "1", d.lastMid, tp, sl);
    }
    else if (signal == "SELL") {
        double tp = d.lastMid - TP_SPREAD_MULTIPLIER * d.avgSpread;
        double sl = d.lastMid + SL_SPREAD_MULTIPLIER * d.avgSpread;
        std::cout << "-> SELL limit+bracket: entry=" << d.lastMid
                  << " qty=1, TP=" << tp << " SL=" << sl
                  << " (avgSpread=" << d.avgSpread << ")\n";
        sendBracketOrder(symbol, "sell", "1", d.lastMid, tp, sl);
    }
    else {
        std::cout << "No order. Majority was NEUTRAL\n";
    }
}

int main() {
    std::ios::sync_with_stdio(false);

    const char* key    = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    if (!key || !secret) {
        logError("ALPACA_API_KEY / ALPACA_API_SECRET not set");
        std::cerr << "ALPACA_API_KEY / ALPACA_API_SECRET not set\n";
        return 1;
    }

    AlpacaWebSocket client(key, secret, {symbol});
    DecisionCore core;
    core.setIntervalSeconds(DECISION_INTERVAL_SECONDS);

    client.setQuoteCallback([](double bid, double ask,
                                double bid_volume, double ask_volume) {
        {
            std::lock_guard<std::mutex> lock(bookMutex);
            symbolOrderBook.push_back(OrderBook{bid, ask, bid_volume, ask_volume});
            if (symbolOrderBook.size() > 500) {
                symbolOrderBook.pop_front();
            }
            newDataAvailable = true;
        }
        bookCv.notify_one();
    });

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::thread feed_thread([&client] { client.run(); });

    // throttle per-quote logging to ~1 line/sec with the previous signal
    static std::string lastLoggedSignal = "NEUTRAL";
    static auto lastQuoteLog = std::chrono::steady_clock::now();

    while (g_stop == 0) {
        std::unique_lock<std::mutex> lock(bookMutex);
        bookCv.wait_for(lock, std::chrono::milliseconds(200),
                        [] { return newDataAvailable || g_stop != 0; });

        if (g_stop != 0) break;

        if (newDataAvailable) {
            newDataAvailable = false;

            if (!symbolOrderBook.empty()) {
                OrderBook latestBook = symbolOrderBook.back();
                lock.unlock();

                QuoteMetrics metrics = core.onQuote(latestBook);

                auto nowLog = std::chrono::steady_clock::now();
                if (nowLog - lastQuoteLog >= std::chrono::milliseconds(1000)) {
                    lastQuoteLog = nowLog;
                    std::cout << nowStr() << " Bid=" << latestBook.bid
                              << " Ask=" << latestBook.ask
                              << " Mid=" << metrics.mid
                              << " Spread=" << metrics.spread
                              << " => " << metrics.signal
                              << (metrics.signal != lastLoggedSignal ? " (changed)" : "")
                              << "\n";
                    lastLoggedSignal = metrics.signal;
                }
            } else {
                lock.unlock();
            }
        }

        // also fires on timeout/spurious wake so we never skip a decision
        if (core.windowElapsed()) {
            if (core.tickCount() > 0) {
                Decision d = core.runDecision();
                std::cout << "== Counting last " << d.tickCount << " signals: "
                          << "BUY=" << d.buyCount
                          << " SELL=" << d.sellCount
                          << " NEUTRAL=" << d.neuCount << " ==\n";
                std::cout << "Expert votes | base=" << expertName(d.base)
                          << " OFI=" << expertName(d.ofi)
                          << " drift=" << expertName(d.drift)
                          << " absorption=" << expertName(d.absorption) << "\n";
                std::cout << "=> Majority: " << expertName(d.combined) << "\n";
                placeOrder(expertName(d.combined), d);
            } else {
                core.resetWindow();
            }
        }
    }

    client.stop();
    feed_thread.join();
    std::cout << "Flow++ stopped.\n";
    return 0;
}
