#include <iostream>
#include <cmath>
#include <deque>
#include <vector>
#include <map>
#include "websocket.h"
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <chrono>
#include <csignal>
#include "order.h"
// Config
const std::string symbol = "SPY";
// Decision runs on a time window (every 5 minutes), not a fixed tick count.
const int DECISION_INTERVAL_SECONDS = 5 * 60; // 5 minutes
// Set to true to place orders.
const bool orderyes = false;

// TP/SL as a multiplier of the average spread over the window (volatility-based).
// R:R of 2:1 (TP further away than SL).
const double TP_SPREAD_MULTIPLIER = 3.0;
const double SL_SPREAD_MULTIPLIER = 1.5;

struct OrderBook {
    double bid;
    double ask;
    double bid_volume;
    double ask_volume;
};

// Incoming quote data
std::deque<OrderBook> Apple_Book_Data;

static std::mutex bookMutex;
static std::condition_variable bookCv;
static bool newDataAvailable = false;

// Buffer for the majority decision over a time window (5 min).
static std::vector<std::string> signalBuffer;
static double spreadSum = 0.0;
static double lastMid = 0.0;
static std::chrono::steady_clock::time_point windowStart =
    std::chrono::steady_clock::now();

// Clean shutdown via SIGINT/SIGTERM.
static volatile std::sig_atomic_t g_stop = 0;
static void handleSignal(int) {
    g_stop = 1;
}

// Function that determines the signal based on a single quote
std::string evaluateSignal(const OrderBook& book) {
    // 1. Basic calculations
    double spread = book.ask - book.bid;
    double mid = (book.bid + book.ask) / 2.0;

    // Safety check to avoid division by zero
    double wmid = mid;

    // Weighted mid formula.
    if ((book.bid_volume + book.ask_volume) > 0) {
        wmid = (book.bid_volume * book.ask + book.ask_volume * book.bid) / (book.bid_volume + book.ask_volume);
    }

    // Bid share of the total volume; guard against zero volumes.
    double Ratio = 0.0;
    if ((book.bid_volume + book.ask_volume) > 0) {
        Ratio = (double)book.bid_volume / (book.bid_volume + book.ask_volume);
    }

    // Print the base data
    std::cout << "Bid: " << book.bid << " | Ask: " << book.ask << " | Spread: " << spread << "\n";
    std::cout << "Mid: " << mid << " | Weighted Mid: " << wmid << "\n";

    // Track for the majority decision
    spreadSum += spread;
    lastMid = mid;

    // 2. Signal logic (based on Weighted Mid vs Mid)
    std::cout << "SIGNAL: ";
    if (wmid > mid && Ratio > 0.60) {
        std::cout << " BUY  (Buyer pressure dominant)\n";
        return "BUY";
    }
    else if (wmid < mid && Ratio < 0.40) {
        std::cout << " SELL  (Seller pressure dominant)\n";
        return "SELL";
    } else {
        std::cout << "NEUTRAL (Volumes in balance)\n";
        return "NEUTRAL";
    }
}

// Counts which signal occurred most often in the buffer.
std::string majoritySignal(const std::vector<std::string>& signals) {
    std::map<std::string, int> counts;
    for (const auto& s : signals) counts[s]++;

    std::string best = "NEUTRAL";
    int bestCount = -1;
    for (const auto& [sig, count] : counts) {
        if (count > bestCount) {
            bestCount = count;
            best = sig;
        }
    }

    std::cout << "== Counting last " << signals.size() << " signals: "
              << "BUY=" << counts["BUY"]
              << " SELL=" << counts["SELL"]
              << " NEUTRAL=" << counts["NEUTRAL"]
              << " => Majority: " << best << " ==\n";

    return best;
}

// Places a bracket order (TP/SL) from the majority signal, using the average
// spread over the window as a volatility measure. Entry is a limit order at
// the current mid, so TP/SL anchor to a known entry price.
void order(const std::string& signal, int tickCount) {
    if (!orderyes) {
        std::cout << "orders are disabled, no orders placed\n";
        return;
    }

    // Tick count varies per window, so divide the spread sum by the real count.
    double avgSpread = tickCount > 0 ? spreadSum / tickCount : 0.0;
    double entry = lastMid;

    if (signal == "BUY") {
        double tp = entry + TP_SPREAD_MULTIPLIER * avgSpread;
        double sl = entry - SL_SPREAD_MULTIPLIER * avgSpread;
        std::cout << "-> BUY limit+bracket: entry=" << entry
                  << " qty=1, TP=" << tp << " SL=" << sl
                  << " (avgSpread=" << avgSpread << ")\n";
        sendBracketOrder(symbol, "buy", "1", entry, tp, sl);
    }
    else if (signal == "SELL") {
        double tp = entry - TP_SPREAD_MULTIPLIER * avgSpread;
        double sl = entry + SL_SPREAD_MULTIPLIER * avgSpread;
        std::cout << "-> SELL limit+bracket: entry=" << entry
                  << " qty=1, TP=" << tp << " SL=" << sl
                  << " (avgSpread=" << avgSpread << ")\n";
        sendBracketOrder(symbol, "sell", "1", entry, tp, sl);
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
        std::cerr << "ALPACA_API_KEY / ALPACA_API_SECRET not set\n";
        return 1;
    }

    AlpacaWebSocket client(key, secret, {symbol});

    client.setQuoteCallback([](double bid, double ask,
                                double bid_volume, double ask_volume) {
        {
            std::lock_guard<std::mutex> lock(bookMutex);
            Apple_Book_Data.push_back(OrderBook{bid, ask, bid_volume, ask_volume});
            if (Apple_Book_Data.size() > 500) {
                Apple_Book_Data.pop_front();
            }
            newDataAvailable = true;
        }
        bookCv.notify_one();
    });

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    std::thread feed_thread([&client] { client.run(); });

    while (g_stop == 0) {
        std::unique_lock<std::mutex> lock(bookMutex);
        bookCv.wait_for(lock, std::chrono::milliseconds(200),
                        [] { return newDataAvailable || g_stop != 0; });

        if (g_stop != 0) break;

        if (newDataAvailable) {
            newDataAvailable = false;

            if (!Apple_Book_Data.empty()) {
                OrderBook latestBook = Apple_Book_Data.back();
                lock.unlock();

                std::cout << "\n-- New quote received --\n";
                std::string signal = evaluateSignal(latestBook);
                signalBuffer.push_back(signal);
            } else {
                lock.unlock();
            }
        }

        // Also runs on timeout/spurious wake so a decision is never skipped.
        auto now = std::chrono::steady_clock::now();
        if (now - windowStart >= std::chrono::seconds(DECISION_INTERVAL_SECONDS)) {
            if (!signalBuffer.empty()) {
                std::string decision = majoritySignal(signalBuffer);
                order(decision, static_cast<int>(signalBuffer.size()));
            }

            // Reset window for the next 5 minutes
            signalBuffer.clear();
            spreadSum = 0.0;
            windowStart = now;
        }
    }

    client.stop();
    feed_thread.join();
    std::cout << "Flow++ stopped.\n";
    return 0;
}
