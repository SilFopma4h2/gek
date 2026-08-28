// position.cpp
#include "position.h"
#include "http_client.h"
#include "logger.h"

#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

namespace pos {

namespace {

// Alpaca reports a position's qty as a string; for a long it's positive,
// for a short it's negative. We expose it as a signed double and let the
// guard decide what to do.
double parseQty(const std::string& s) {
    try { return std::stod(s); } catch (...) { return 0.0; }
}

bool orderIsWorking(const std::string& status) {
    // Alpaca's "working" set: orders that may still execute. "filled" and
    // "canceled"/"expired"/"rejected" are terminal.
    return status == "new"
        || status == "accepted"
        || status == "pending_new"
        || status == "partially_filled"
        || status == "replaced";
}

} // namespace

bool PendingOrder::isWorking() const { return orderIsWorking(status); }

PositionGuard::PositionGuard(std::string symbol, std::chrono::seconds ttl)
    : symbol_(std::move(symbol)), ttl_(ttl) {}

bool PositionGuard::refresh() {
    Snapshot s;
    s.symbol = symbol_;

    // /v2/positions returns [] when the account has no positions; a 404
    // means the same thing for paper. Treat both as "no positions".
    auto posResp = http::get("/v2/positions");
    if (posResp.ok) {
        try {
            auto j = json::parse(posResp.body);
            if (j.is_array()) {
                for (const auto& p : j) {
                    if (!p.contains("symbol") || !p["symbol"].is_string()) continue;
                    if (p["symbol"].get<std::string>() != symbol_) continue;
                    Position pos;
                    pos.symbol = symbol_;
                    if (p.contains("qty") && p["qty"].is_string()) {
                        pos.qty = parseQty(p["qty"].get<std::string>());
                    }
                    if (p.contains("avg_entry_price")
                        && p["avg_entry_price"].is_string()) {
                        try {
                            pos.avgEntryPrice =
                                std::stod(p["avg_entry_price"].get<std::string>());
                        } catch (...) { pos.avgEntryPrice = 0.0; }
                    }
                    if (p.contains("side") && p["side"].is_string()) {
                        pos.side = p["side"].get<std::string>();
                    }
                    s.positions.push_back(pos);
                }
            }
        } catch (const std::exception& e) {
            s.error = std::string("positions parse: ") + e.what();
        }
    } else if (posResp.status == 404) {
        // 404 == no positions, that's fine
    } else if (posResp.status != 0) {
        s.error = "positions HTTP " + std::to_string(posResp.status);
    }

    // /v2/orders returns all open orders for the account; we filter to
    // ones for our symbol. status query defaults to "open" on Alpaca.
    auto ordResp = http::get("/v2/orders?status=open&limit=50");
    if (ordResp.ok) {
        try {
            auto j = json::parse(ordResp.body);
            if (j.is_array()) {
                for (const auto& o : j) {
                    if (!o.contains("symbol") || !o["symbol"].is_string()) continue;
                    if (o["symbol"].get<std::string>() != symbol_) continue;
                    PendingOrder po;
                    if (o.contains("id") && o["id"].is_string())
                        po.id = o["id"].get<std::string>();
                    po.symbol = symbol_;
                    if (o.contains("side") && o["side"].is_string())
                        po.side = o["side"].get<std::string>();
                    if (o.contains("type") && o["type"].is_string())
                        po.type = o["type"].get<std::string>();
                    if (o.contains("status") && o["status"].is_string())
                        po.status = o["status"].get<std::string>();
                    if (o.contains("qty") && o["qty"].is_string())
                        po.qty = parseQty(o["qty"].get<std::string>());
                    if (o.contains("limit_price")
                        && o["limit_price"].is_string()) {
                        try {
                            po.limitPrice =
                                std::stod(o["limit_price"].get<std::string>());
                        } catch (...) { po.limitPrice = 0.0; }
                    }
                    s.orders.push_back(po);
                }
            }
        } catch (const std::exception& e) {
            s.error = std::string("orders parse: ") + e.what();
        }
    } else if (ordResp.status != 0 && ordResp.status != 404) {
        if (s.error.empty()) s.error = "orders HTTP " + std::to_string(ordResp.status);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap_ = std::move(s);
        snap_.valid = true;
        fetched_ = std::chrono::steady_clock::now();
    }
    return true;
}

Snapshot PositionGuard::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snap_;
}

bool PositionGuard::isStale() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!snap_.valid) return true;
    auto age = std::chrono::steady_clock::now() - fetched_;
    return age > ttl_;
}

GuardVerdict PositionGuard::canPlaceOrder(const std::string& side) const {
    GuardVerdict v;
    v.allowed = true;
    Snapshot s = snapshot();

    // any open position in the same direction blocks a new bracket
    for (const auto& p : s.positions) {
        if (p.qty == 0.0) continue;
        if (side == "buy" && p.qty > 0.0) {
            v.allowed = false;
            v.reason = "Already long " + std::to_string(p.qty)
                     + " " + p.symbol + " (entry $" + std::to_string(p.avgEntryPrice) + ")";
            return v;
        }
        if (side == "sell" && p.qty < 0.0) {
            v.allowed = false;
            v.reason = "Already short " + std::to_string(p.qty)
                     + " " + p.symbol;
            return v;
        }
    }

    // any working order in the same direction blocks. The position.cpp
    // refresh() already filters orders by symbol, so the guard does not
    // need to filter again here — but defensive check is cheap.
    for (const auto& o : s.orders) {
        if (!o.isWorking()) continue;
        if (o.symbol != s.symbol) continue;
        if (o.side == side) {
            v.allowed = false;
            v.reason = "Pending " + o.side + " " + o.type
                     + " order for " + o.symbol + " (status=" + o.status + ")";
            return v;
        }
    }

    return v;
}

void PositionGuard::setForTesting(Snapshot snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    snap_ = std::move(snap);
    snap_.valid = true;
    fetched_ = std::chrono::steady_clock::now();
}

} // namespace pos
