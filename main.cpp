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
#include "experts.h"
// config
const std::string symbol = "SPY";
// decide on a time window (5 min), not on a tick count
const int DECISION_INTERVAL_SECONDS = 5 * 60; // 5 minutes
// set to true to actually place orders
const bool orderyes = true;

// tp/sl sized off the average spread in the window
// 2:1 rr, tp further away than sl
const double TP_SPREAD_MULTIPLIER = 3.0;
const double SL_SPREAD_MULTIPLIER = 1.5;

struct OrderBook {
    double bid;
    double ask;
    double bid_volume;
    double ask_volume;
};

// incoming quotes
std::deque<OrderBook> symbolOrderBook;

static std::mutex bookMutex;
static std::condition_variable bookCv;
static bool newDataAvailable = false;

// running tallies for the current window, O(1) per quote.
// replaces the old signalBuffer vector that grew unbounded all window long.
static int buyCount = 0;
static int sellCount = 0;
static int neuCount = 0;
static int windowTicks = 0;
static double spreadSum = 0.0;
static double lastMid = 0.0;
static FlowState g_flow;
static std::chrono::steady_clock::time_point windowStart =
    std::chrono::steady_clock::now();

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

// signal for a single quote; pure computation, no stdout (printing is the
// CPU/IO heavy part at these quote rates, so the caller throttles it)
struct QuoteMetrics {
    double mid, wmid, spread, ratio;
    std::string signal;
};

QuoteMetrics evaluateSignal(const OrderBook& book) {
    QuoteMetrics m;
    m.spread = book.ask - book.bid;
    m.mid = (book.bid + book.ask) / 2.0;
    m.wmid = m.mid;
    m.ratio = 0.0;
    const double totalSize = book.bid_volume + book.ask_volume;
    if (totalSize > 0.0) {
        m.wmid = (book.bid_volume * book.ask + book.ask_volume * book.bid) / totalSize;
        m.ratio = (double)book.bid_volume / totalSize;
    }

    // the IEX feed often sends bid/ask sizes of 0, which makes wmid == mid and
    // Ratio == 0.0 (-> the volume rules below would ALWAYS return NEUTRAL).
    // With no size info, fall back to mid-price momentum so the bot still
    // produces a meaningful signal.
    static double lastMidForMomentum = 0.0;
    static bool hasMidForMomentum = false;

    if (totalSize > 0.0) {
        lastMidForMomentum = m.mid;
        hasMidForMomentum = true;
        if (m.wmid > m.mid && m.ratio > 0.60) m.signal = "BUY";
        else if (m.wmid < m.mid && m.ratio < 0.40) m.signal = "SELL";
        else m.signal = "NEUTRAL";
        return m;
    }

    if (hasMidForMomentum) {
        const double momentum = m.mid - lastMidForMomentum;
        const double tick = std::max(0.01, m.spread * 0.5);
        lastMidForMomentum = m.mid;
        if (momentum > tick) m.signal = "BUY";
        else if (momentum < -tick) m.signal = "SELL";
        else m.signal = "NEUTRAL";
        return m;
    }
    lastMidForMomentum = m.mid;
    hasMidForMomentum = true;
    m.signal = "NEUTRAL";
    return m;
}

// most common signal from the window tallies (BUY > NEUTRAL > SELL on a draw)
std::string majorityFromCounts(int buy, int sell, int neu) {
    int bestCount = std::max({buy, sell, neu});
    if (bestCount <= 0) return "NEUTRAL";
    if (buy == bestCount) return "BUY";
    if (neu == bestCount) return "NEUTRAL";
    return "SELL";
}

// bracket order (tp/sl) from the majority signal; entry is a limit at
// the current mid so tp/sl anchor to a known price
void order(const std::string& signal, int tickCount) {
    if (!orderyes) {
        std::cout << "orders are disabled, no orders placed\n";
        return;
    }

    // window tick count varies, so divide by the real count
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

                QuoteMetrics metrics = evaluateSignal(latestBook);
                updateFlowState(g_flow, latestBook.bid, latestBook.ask,
                                latestBook.bid_volume, latestBook.ask_volume);
                if (metrics.signal == "BUY")      buyCount++;
                else if (metrics.signal == "SELL") sellCount++;
                else                               neuCount++;
                windowTicks++;

                // throttle per-quote logging to ~1 line/sec with the previous
                // signal, so high quote rates don't choke on stdout
                static double lastLoggedMid = -1.0;
                static std::string lastLoggedSignal = "NEUTRAL";
                auto nowLog = std::chrono::steady_clock::now();
                static auto lastQuoteLog = nowLog;
                if (nowLog - lastQuoteLog >= std::chrono::milliseconds(1000)) {
                    lastQuoteLog = nowLog;
                    std::cout << nowStr() << " Bid=" << latestBook.bid
                              << " Ask=" << latestBook.ask
                              << " Mid=" << metrics.mid
                              << " Spread=" << metrics.spread
                              << " => " << metrics.signal
                              << (metrics.signal != lastLoggedSignal ? " (changed)" : "")
                              << "\n";
                    lastLoggedMid = metrics.mid;
                    lastLoggedSignal = metrics.signal;
                }
            } else {
                lock.unlock();
            }
        }

        // also fires on timeout/spurious wake so we never skip a decision
        auto now = std::chrono::steady_clock::now();
        if (now - windowStart >= std::chrono::seconds(DECISION_INTERVAL_SECONDS)) {
            if (windowTicks > 0 || g_flow.tickCount > 0) {
                // keep the legacy order() inputs in sync with the flow state
                spreadSum = g_flow.spreadSum;
                lastMid = g_flow.lastMid;

                const std::string baseStr = majorityFromCounts(buyCount, sellCount, neuCount);
                const ExpertSignal baseVote = baseStr == "BUY"  ? ExpertSignal::BUY
                                            : baseStr == "SELL" ? ExpertSignal::SELL
                                                                : ExpertSignal::NEUTRAL;
                const ExpertSignal ofiVote = ofiSignal(g_flow);
                const ExpertSignal driftVote = driftSignal(g_flow);
                const ExpertSignal absVote = absorptionSignal(g_flow);

                std::cout << "== Counting last " << windowTicks << " signals: "
                          << "BUY=" << buyCount
                          << " SELL=" << sellCount
                          << " NEUTRAL=" << neuCount << " ==\n";
                std::cout << "Expert votes | base=" << expertName(baseVote)
                          << " OFI=" << expertName(ofiVote)
                          << " drift=" << expertName(driftVote)
                          << " absorption=" << expertName(absVote) << "\n";

                std::string decision = expertName(
                    combinedDecision(baseVote, ofiVote, driftVote, absVote));
                std::cout << "=> Majority: " << decision << "\n";
                order(decision, windowTicks);
            }

            // start a fresh window
            buyCount = sellCount = neuCount = windowTicks = 0;
            spreadSum = 0.0;
            g_flow = FlowState{};
            windowStart = now;
        }
    }

    client.stop();
    feed_thread.join();
    std::cout << "Flow++ stopped.\n";
    return 0;
}
