// order.h
#pragma once
#include <string>
#include <functional>

// optional log callback for the gui; without one we fall back to cout/cerr
using OrderLogCallback = std::function<void(const std::string&, bool /*isError*/)>;
void setOrderLogCallback(OrderLogCallback cb);

// plain market order, no tp/sl (kept for standalone calls)
void sendOrder(const std::string& symbol, const std::string& side, const std::string& qty);

// bracket order: limit entry + take-profit + stop-loss in one call.
// all three prices are absolute, not offsets.
// NOTE: Alpaca rejects bracket orders on fractional qty,
// so qty has to be a whole number (e.g. "1").
void sendBracketOrder(const std::string& symbol,
                       const std::string& side,
                       const std::string& qty,
                       double entryPrice,
                       double takeProfitPrice,
                       double stopLossPrice);
