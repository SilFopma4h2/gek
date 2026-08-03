// order.h
#pragma once
#include <string>

// Simpele market order zonder TP/SL (blijft bestaan voor losse calls)
void sendOrder(const std::string& symbol, const std::string& side, const std::string& qty);

// Bracket order: market entry + take-profit + stop-loss in één keer.
// takeProfitPrice en stopLossPrice zijn absolute prijzen (geen offsets).
// LET OP: Alpaca ondersteunt geen bracket orders op fractional qty,
// dus qty moet hier een heel getal zijn (bv. "1").
void sendBracketOrder(const std::string& symbol,
                       const std::string& side,
                       const std::string& qty,
                       double takeProfitPrice,
                       double stopLossPrice);