// position.h
//
// Tracks open positions and pending orders for one symbol via the Alpaca
// REST API. A short-lived cache (TTL configurable) avoids hammering the
// API on every decision window while still picking up fills within a
// second or two.
//
// Thread model: refresh() is called from the main thread (right after
// runDecision() decides, or from a background poll). Reads via snapshot()
// and canPlaceOrder() take the internal mutex and return a copy.
//
// The guard logic prevents the bot from piling up bracket orders across
// windows. If the same BUY signal fires two windows in a row, the second
// one would open a second long bracket while the first is still active.
// Alpaca will sometimes accept that (two separate long positions on the
// same symbol) and sometimes reject it; either way it's a bug, so we
// gate on the local cache.
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pos {

struct Position {
    std::string symbol;
    double qty = 0.0;           // negative = short
    double avgEntryPrice = 0.0;
    std::string side;           // "long" or "short"
};

struct PendingOrder {
    std::string id;
    std::string symbol;
    std::string side;           // "buy" or "sell"
    std::string type;           // "limit" / "market" / "stop" / "stop_limit"
    std::string status;         // "new" / "accepted" / "partially_filled" / "pending_new" / ...
    double qty = 0.0;
    double limitPrice = 0.0;
    // true if Alpaca reports the order is still in a working state
    bool isWorking() const;
};

struct Snapshot {
    bool valid = false;
    std::string symbol;
    std::vector<Position> positions;
    std::vector<PendingOrder> orders;
    std::string error;          // non-empty if the last refresh failed
};

struct GuardVerdict {
    bool allowed = false;       // true if the order is safe to send
    std::string reason;         // human-readable explanation when blocked
};

// cache TTL in seconds; small enough that fills show up within a window
// without re-querying on every quote.
constexpr std::chrono::seconds DEFAULT_TTL{3};

class PositionGuard {
public:
    explicit PositionGuard(std::string symbol,
                           std::chrono::seconds ttl = DEFAULT_TTL);

    // re-query the REST API; safe to call from any thread but the caller
    // should serialize it (one refresh in flight at a time is enough).
    bool refresh();

    // last fetched snapshot (may be empty/stale if refresh never ran)
    Snapshot snapshot() const;

    // did the cache age out? used by the GUI to warn the user.
    bool isStale() const;

    // decide whether a new order of the given side is safe. Reasons for
    // blocking: existing open position in the same direction, or a
    // working bracket order in the same direction.
    GuardVerdict canPlaceOrder(const std::string& side) const;

    // helpers used by tests to inject state without going through the API
    void setForTesting(Snapshot snap);

    const std::string& symbol() const { return symbol_; }

private:
    std::string symbol_;
    std::chrono::seconds ttl_;
    mutable std::mutex mutex_;
    Snapshot snap_{};
    std::chrono::steady_clock::time_point fetched_{};
};

} // namespace pos
