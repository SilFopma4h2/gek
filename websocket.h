#pragma once
#include <string>
#include <vector>
#include <functional>
#include <atomic>

// Callback-signatuur: bid, ask, bid_volume, ask_volume
// Zo blijft deze module onafhankelijk van de OrderBook-struct in main.cpp
using QuoteCallback = std::function<void(double bid, double ask,
                                          double bid_volume, double ask_volume)>;

// Statusmeldingen voor een UI (verbinden, geauthenticeerd, fouten, reconnect, ...)
using StatusCallback = std::function<void(const std::string&)>;

class AlpacaWebSocket {
public:
    AlpacaWebSocket(std::string api_key,
                     std::string api_secret,
                     std::vector<std::string> symbols);

    // Wordt aangeroepen telkens er een nieuwe quote binnenkomt
    void setQuoteCallback(QuoteCallback cb);

    // Wordt aangeroepen bij statusveranderingen (verbinden, fouten, ...)
    void setStatusCallback(StatusCallback cb);

    // Blokkerende call: opent verbinding, auth, subscribe, en verwerkt events
    void run();

    // Vraagt een nette shutdown aan; run() stopt binnenkort (max. een paar seconden).
    void stop();

private:
    void notifyStatus(const std::string& msg) const;

    std::string api_key_;
    std::string api_secret_;
    std::vector<std::string> symbols_;
    QuoteCallback callback_;
    StatusCallback status_cb_;
    std::atomic<bool> stop_requested_{false};
    bool connectAndListen();
};