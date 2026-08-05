// order.h
#pragma once
#include <string>
#include <functional>

// Optionele log-callback voor een UI (GUI). Zonder callback wordt er
// naar std::cout / std::cerr gelogd, zodat de console-gedrag hetzelfde blijft.
using OrderLogCallback = std::function<void(const std::string&, bool /*isError*/)>;
void setOrderLogCallback(OrderLogCallback cb);

// Simpele market order zonder TP/SL (blijft bestaan voor losse calls)
void sendOrder(const std::string& symbol, const std::string& side, const std::string& qty);

// Bracket order: limit entry + take-profit + stop-loss in één keer.
// entryPrice, takeProfitPrice en stopLossPrice zijn absolute prijzen (geen offsets).
// LET OP: Alpaca ondersteunt geen bracket orders op fractional qty,
// dus qty moet hier een heel getal zijn (bv. "1").
void sendBracketOrder(const std::string& symbol,
                       const std::string& side,
                       const std::string& qty,
                       double entryPrice,
                       double takeProfitPrice,
                       double stopLossPrice);