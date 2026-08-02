#pragma once
#include <string>
#include <functional>

// Callback-signatuur: bid, ask, bid_volume, ask_volume
// Zo blijft deze module onafhankelijk van de OrderBook-struct in main.cpp
using QuoteCallback = std::function<void(double bid, double ask,
                                          double bid_volume, double ask_volume)>;

class AlpacaWebSocket {
public:
    AlpacaWebSocket(std::string api_key,
                     std::string api_secret,
                     std::vector<std::string> symbols);

    // Wordt aangeroepen telkens er een nieuwe quote binnenkomt
    void setQuoteCallback(QuoteCallback cb);

    // Blokkerende call: opent verbinding, auth, subscribe, en verwerkt events
    void run();

private:
    std::string api_key_;
    std::string api_secret_;
    std::vector<std::string> symbols_;
    QuoteCallback callback_;
};