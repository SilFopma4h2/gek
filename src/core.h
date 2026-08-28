// core.h
//
// Shared signal/state logic for the console (main.cpp) and GUI (gui.cpp)
// front-ends. One class owns the running tallies (signal counts, spread sum,
// last mid, flow state for the experts, momentum-fallback reference) and the
// window-timer state. Both front-ends call onQuote() per incoming quote and
// either runDecision() (when windowElapsed() is true) or schedule it via a
// background thread.
//
// All state inside DecisionCore is single-threaded: the caller is expected
// to invoke onQuote() and runDecision() from the same thread. The GUI
// guarantees this because it pumps ImGui from the main thread; the console
// version does the same. The websocket callback only delivers quotes into
// a queue the main thread later drains via onQuote().
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>

#include "experts.h"

struct OrderBook {
    double bid = 0.0;
    double ask = 0.0;
    double bid_volume = 0.0;
    double ask_volume = 0.0;
};

struct QuoteMetrics {
    double mid = 0.0;
    double wmid = 0.0;
    double spread = 0.0;
    double ratio = 0.0;
    std::string signal = "NEUTRAL";
};

// shared signal thresholds (volume ratio and momentum tick size)
struct SignalParams {
    double buyRatio = 0.60;   // bid_volume / total >= 0.60 -> BUY
    double sellRatio = 0.40;  // bid_volume / total <= 0.40 -> SELL
    double minTick = 0.01;    // minimum price move considered a momentum tick
    double tickFrac = 0.5;    // fraction of spread treated as a momentum tick
};

// result of one decision window
struct Decision {
    ExpertSignal combined = ExpertSignal::NEUTRAL;
    ExpertSignal base = ExpertSignal::NEUTRAL;
    ExpertSignal ofi = ExpertSignal::NEUTRAL;
    ExpertSignal drift = ExpertSignal::NEUTRAL;
    ExpertSignal absorption = ExpertSignal::NEUTRAL;

    int buyCount = 0;
    int sellCount = 0;
    int neuCount = 0;
    int tickCount = 0;
    double avgSpread = 0.0;
    double lastMid = 0.0;
};

// evaluate one quote, no state on its own
QuoteMetrics evaluateSignal(const OrderBook& book,
                            const SignalParams& params,
                            double& prevMid,
                            bool& hasPrevMid);

// most common signal from the window tallies (BUY > NEUTRAL > SELL on a draw)
std::string majorityFromCounts(int buy, int sell, int neu);

class DecisionCore {
public:
    DecisionCore();
    explicit DecisionCore(SignalParams params);

    // process one quote; updates signal counts, flow state, momentum state
    QuoteMetrics onQuote(const OrderBook& q);

    // true when the current decision window has elapsed
    bool windowElapsed() const;

    // force-fire a decision regardless of the timer
    Decision runDecision();

    // reset the window timer and all running tallies
    void resetWindow();

    // window length
    int intervalSeconds() const { return intervalSeconds_; }
    void setIntervalSeconds(int s) { intervalSeconds_ = s; }

    // signal / expert parameters (live-tweakable, hence the mutex)
    SignalParams signalParams() const;
    void setSignalParams(SignalParams p);

    Thresholds thresholds() const;
    void setThresholds(Thresholds t);

    // read-only view of the running tallies (for UI/logging)
    int buyCount() const;
    int sellCount() const;
    int neuCount() const;
    int tickCount() const;
    double spreadSum() const;
    double lastMid() const;
    const FlowState& flow() const { return flow_; }
    std::string lastSignal() const { return lastSignal_; }

private:
    int intervalSeconds_ = 5 * 60;
    std::chrono::steady_clock::time_point windowStart_ =
        std::chrono::steady_clock::now();

    int buyCount_ = 0;
    int sellCount_ = 0;
    int neuCount_ = 0;
    int tickCount_ = 0;
    double spreadSum_ = 0.0;
    double lastMid_ = 0.0;
    double prevMidForMomentum_ = 0.0;
    bool hasPrevMid_ = false;
    std::string lastSignal_ = "NEUTRAL";

    FlowState flow_{};

    // mutable so const accessors can lock
    mutable std::mutex paramsMutex_;
    SignalParams signalParams_;
    Thresholds thresholds_;
};
