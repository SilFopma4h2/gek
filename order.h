// order.h
#pragma once
#include <string>
#include <functional>

// Optional log callback for a UI (GUI). Without a callback, messages are
// logged to std::cout / std::cerr, so console behavior stays the same.
using OrderLogCallback = std::function<void(const std::string&, bool /*isError*/)>;
void setOrderLogCallback(OrderLogCallback cb);

// Simple market order without TP/SL (kept for standalone calls)
void sendOrder(const std::string& symbol, const std::string& side, const std::string& qty);

// Bracket order: limit entry + take-profit + stop-loss in one call.
// entryPrice, takeProfitPrice and stopLossPrice are absolute prices (no offsets).
// NOTE: Alpaca does not support bracket orders on fractional qty,
// so qty must be a whole number (e.g. "1").
void sendBracketOrder(const std::string& symbol,
                       const std::string& side,
                       const std::string& qty,
                       double entryPrice,
                       double takeProfitPrice,
                       double stopLossPrice);
