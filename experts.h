// experts.h
//
// Order-flow experts that vote alongside the base signal (weighted-mid vs
// mid + bid ratio) in main.cpp / gui.cpp. Every expert consumes only the
// fields the websocket already delivers (bid, ask, bid volume, ask volume)
// and produces a per-window BUY / SELL / NEUTRAL vote from running tallies
// that are reset together with the existing decision window.
//
// The final decision is a simple majority over the four votes; on a 2-2 tie
// it falls back to the base expert so the incumbent behaviour is preserved
// whenever the new experts are uncertain.
#pragma once

#include <algorithm>

struct FlowQuote {
    double bid = 0.0;
    double ask = 0.0;
    double bid_volume = 0.0;
    double ask_volume = 0.0;
};

enum class ExpertSignal {
    NEUTRAL,
    BUY,
    SELL
};

// running tallies for the new experts, one per symbol, reset every window
struct FlowState {
    // previous quote for the size-delta based experts
    bool hasPrev = false;
    FlowQuote prev{};

    // shared window totals (mirror the base expert's spreadSum / lastMid)
    double spreadSum = 0.0;
    double lastMid = 0.0;
    double tickCount = 0.0;
    double totalVolume = 0.0;

    // OFI: cumulative signed order-flow imbalance
    double ofiSum = 0.0;

    // microprice drift: cumulative (weighted-mid - mid)
    double driftSum = 0.0;

    // absorption / spoofing: passively added vs cancelled size per side
    double absorbBid = 0.0;
    double absorbAsk = 0.0;
    double cancelBid = 0.0;
    double cancelAsk = 0.0;
};

// tunable thresholds (runtime-adjustable from the GUI)
inline double OFI_THRESHOLD = 0.05;      // frac of total volume
inline double DRIFT_THRESHOLD = 0.10;    // frac of average spread
inline double ABSORPTION_THRESHOLD = 0.05; // frac of total volume

// call once per quote, before reading the votes
inline void updateFlowState(FlowState& s, double bid, double ask,
                            double bid_volume, double ask_volume) {
    const double vol = bid_volume + ask_volume;
    const double spread = ask - bid;
    const double mid = (bid + ask) * 0.5;
    double wmid = mid;
    if (vol > 0.0) {
        wmid = (bid_volume * ask + ask_volume * bid) / vol;
    }

    s.spreadSum += spread;
    s.lastMid = mid;
    s.tickCount += 1.0;
    s.totalVolume += vol;
    s.driftSum += (wmid - mid);

    if (s.hasPrev) {
        const double db = bid_volume - s.prev.bid_volume;
        const double da = ask_volume - s.prev.ask_volume;

        // Order Flow Imbalance (Cont-Kukanov-Stoikov flavour): positive when
        // bid size grows while the bid price holds/rises, negative when ask
        // size grows while the ask price holds/falls.
        s.ofiSum += (bid >= s.prev.bid ? db : 0.0) - (ask <= s.prev.ask ? da : 0.0);

        // Liquidity absorption: size quietly added at a held price is passive
        // commitment; size removed at a held price is a cancellation/pull.
        const bool bidHeld = bid >= s.prev.bid;
        const bool askHeld = ask <= s.prev.ask;
        if (bidHeld) {
            if (db > 0.0)      s.absorbBid += db;
            else if (db < 0.0) s.cancelBid += -db;
        }
        if (askHeld) {
            if (da > 0.0)      s.absorbAsk += da;
            else if (da < 0.0) s.cancelAsk += -da;
        }
    }
    s.prev = {bid, ask, bid_volume, ask_volume};
    s.hasPrev = true;
}

// Expert 2 - Order Flow Imbalance: net signed size flow, normalised by volume.
inline ExpertSignal ofiSignal(const FlowState& s) {
    if (s.totalVolume <= 0.0) return ExpertSignal::NEUTRAL;
    const double n = s.ofiSum / s.totalVolume;
    if (n > OFI_THRESHOLD) return ExpertSignal::BUY;
    if (n < -OFI_THRESHOLD) return ExpertSignal::SELL;
    return ExpertSignal::NEUTRAL;
}

// Expert 3 - Microprice Drift: cumulative weighted-mid pressure, expressed as
// a fraction of the average spread so it is scale-free across tick rates.
inline ExpertSignal driftSignal(const FlowState& s) {
    if (s.tickCount <= 0.0) return ExpertSignal::NEUTRAL;
    const double avgSpread = s.spreadSum / s.tickCount;
    if (avgSpread <= 0.0) return ExpertSignal::NEUTRAL;
    const double n = s.driftSum / (s.tickCount * avgSpread);
    if (n > DRIFT_THRESHOLD) return ExpertSignal::BUY;
    if (n < -DRIFT_THRESHOLD) return ExpertSignal::SELL;
    return ExpertSignal::NEUTRAL;
}

// Expert 4 - Liquidity Absorption / Spoof Index: net passive commitment
// (added minus cancelled) on the bid vs the ask, normalised by volume.
inline ExpertSignal absorptionSignal(const FlowState& s) {
    if (s.totalVolume <= 0.0) return ExpertSignal::NEUTRAL;
    const double pressure = ((s.absorbBid - s.cancelBid) -
                             (s.absorbAsk - s.cancelAsk)) / s.totalVolume;
    if (pressure > ABSORPTION_THRESHOLD) return ExpertSignal::BUY;
    if (pressure < -ABSORPTION_THRESHOLD) return ExpertSignal::SELL;
    return ExpertSignal::NEUTRAL;
}

// majority over the four votes; on a draw the incumbent base expert decides,
// otherwise NEUTRAL wins (stays out of the market when experts disagree).
inline ExpertSignal combinedDecision(ExpertSignal base, ExpertSignal ofi,
                                     ExpertSignal drift, ExpertSignal absorption) {
    const int buy = (base == ExpertSignal::BUY) + (ofi == ExpertSignal::BUY) +
                    (drift == ExpertSignal::BUY) + (absorption == ExpertSignal::BUY);
    const int sell = (base == ExpertSignal::SELL) + (ofi == ExpertSignal::SELL) +
                     (drift == ExpertSignal::SELL) + (absorption == ExpertSignal::SELL);
    const int neu = 4 - buy - sell;

    if (buy > sell && buy > neu) return ExpertSignal::BUY;
    if (sell > buy && sell > neu) return ExpertSignal::SELL;

    if (buy == sell && base != ExpertSignal::NEUTRAL) return base;
    return ExpertSignal::NEUTRAL;
}

inline const char* expertName(ExpertSignal e) {
    switch (e) {
        case ExpertSignal::BUY:  return "BUY";
        case ExpertSignal::SELL: return "SELL";
        default:                 return "NEUTRAL";
    }
}
