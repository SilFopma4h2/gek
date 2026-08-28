// core.cpp
#include "core.h"

#include <algorithm>
#include <cmath>

QuoteMetrics evaluateSignal(const OrderBook& book,
                            const SignalParams& params,
                            double& prevMid,
                            bool& hasPrevMid) {
    QuoteMetrics m;
    m.spread = book.ask - book.bid;
    m.mid = (book.bid + book.ask) / 2.0;
    m.wmid = m.mid;
    m.ratio = 0.0;

    const double totalSize = book.bid_volume + book.ask_volume;
    if (totalSize > 0.0) {
        m.wmid = (book.bid_volume * book.ask + book.ask_volume * book.bid) / totalSize;
        m.ratio = book.bid_volume / totalSize;
        prevMid = m.mid;
        hasPrevMid = true;

        if (m.wmid > m.mid && m.ratio > params.buyRatio) m.signal = "BUY";
        else if (m.wmid < m.mid && m.ratio < params.sellRatio) m.signal = "SELL";
        else m.signal = "NEUTRAL";
        return m;
    }

    // IEX feed often sends zero sizes; wmid==mid and ratio==0 there, so the
    // volume rules would always return NEUTRAL. With no size info, fall back
    // to mid-price momentum so the bot still produces a meaningful signal.
    if (hasPrevMid) {
        const double momentum = m.mid - prevMid;
        const double tick = std::max(params.minTick, m.spread * params.tickFrac);
        prevMid = m.mid;
        if (momentum > tick) m.signal = "BUY";
        else if (momentum < -tick) m.signal = "SELL";
        else m.signal = "NEUTRAL";
        return m;
    }
    prevMid = m.mid;
    hasPrevMid = true;
    m.signal = "NEUTRAL";
    return m;
}

std::string majorityFromCounts(int buy, int sell, int neu) {
    int bestCount = std::max({buy, sell, neu});
    if (bestCount <= 0) return "NEUTRAL";
    if (buy == bestCount) return "BUY";
    if (neu == bestCount) return "NEUTRAL";
    return "SELL";
}

DecisionCore::DecisionCore() = default;

DecisionCore::DecisionCore(SignalParams params) : signalParams_(params) {}

QuoteMetrics DecisionCore::onQuote(const OrderBook& q) {
    SignalParams params;
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        params = signalParams_;
    }

    QuoteMetrics m = evaluateSignal(q, params, prevMidForMomentum_, hasPrevMid_);

    spreadSum_ += m.spread;
    lastMid_ = m.mid;
    updateFlowState(flow_, q.bid, q.ask, q.bid_volume, q.ask_volume);
    tickCount_++;
    if (m.signal == "BUY")       buyCount_++;
    else if (m.signal == "SELL") sellCount_++;
    else                         neuCount_++;
    lastSignal_ = m.signal;
    return m;
}

bool DecisionCore::windowElapsed() const {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(
               now - windowStart_).count() >= intervalSeconds_;
}

Decision DecisionCore::runDecision() {
    Decision d;
    d.buyCount = buyCount_;
    d.sellCount = sellCount_;
    d.neuCount = neuCount_;
    d.tickCount = tickCount_;
    d.lastMid = lastMid_;
    d.avgSpread = tickCount_ > 0 ? spreadSum_ / tickCount_ : 0.0;

    Thresholds thr;
    {
        std::lock_guard<std::mutex> lock(paramsMutex_);
        thr = thresholds_;
    }

    const std::string baseStr = majorityFromCounts(buyCount_, sellCount_, neuCount_);
    d.base = baseStr == "BUY"  ? ExpertSignal::BUY
           : baseStr == "SELL" ? ExpertSignal::SELL
                               : ExpertSignal::NEUTRAL;
    d.ofi = ofiSignal(flow_, thr);
    d.drift = driftSignal(flow_, thr);
    d.absorption = absorptionSignal(flow_, thr);
    d.combined = combinedDecision(d.base, d.ofi, d.drift, d.absorption);

    resetWindow();
    return d;
}

void DecisionCore::resetWindow() {
    buyCount_ = 0;
    sellCount_ = 0;
    neuCount_ = 0;
    tickCount_ = 0;
    spreadSum_ = 0.0;
    // intentionally keep prevMidForMomentum_ / hasPrevMid_: momentum
    // tracking is per-stream, not per-window
    flow_ = FlowState{};
    windowStart_ = std::chrono::steady_clock::now();
}

SignalParams DecisionCore::signalParams() const {
    std::lock_guard<std::mutex> lock(paramsMutex_);
    return signalParams_;
}

void DecisionCore::setSignalParams(SignalParams p) {
    std::lock_guard<std::mutex> lock(paramsMutex_);
    signalParams_ = p;
}

Thresholds DecisionCore::thresholds() const {
    std::lock_guard<std::mutex> lock(paramsMutex_);
    return thresholds_;
}

void DecisionCore::setThresholds(Thresholds t) {
    std::lock_guard<std::mutex> lock(paramsMutex_);
    thresholds_ = t;
}

int DecisionCore::buyCount() const  { return buyCount_;  }
int DecisionCore::sellCount() const { return sellCount_; }
int DecisionCore::neuCount() const  { return neuCount_;  }
int DecisionCore::tickCount() const { return tickCount_; }
double DecisionCore::spreadSum() const { return spreadSum_; }
double DecisionCore::lastMid() const   { return lastMid_;   }
