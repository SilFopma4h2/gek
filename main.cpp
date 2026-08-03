#include <iostream>
#include <cmath>
#include <deque>
#include <vector>
#include <map>
#include "websocket.h"
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include "order.h"
//Hier de confg
const std::string symbol = "SPY";
const int TICKS_PER_DECISION = 60;
//Hier zet je aan of je een order wil plaatsen.
const bool orderyes = false;

// TP/SL als multiplier van de gemiddelde spread over het venster (volatiliteit-gebaseerd).
// R:R van 2:1 (TP verder weg dan SL).
const double TP_SPREAD_MULTIPLIER = 3.0;
const double SL_SPREAD_MULTIPLIER = 1.5;

struct OrderBook {
    double bid;
    double ask;
    double bid_volume;
    double ask_volume;
};

//Hier word de data opgeslagen
std::deque<OrderBook> Apple_Book_Data;

static std::mutex bookMutex;
static std::condition_variable bookCv;
static bool newDataAvailable = false;

// Buffer voor de meerderheids-beslissing over TICKS_PER_DECISION signalen.
static std::vector<std::string> signalBuffer;
static double spreadSum = 0.0;
static double lastMid = 0.0;

// Functie die het signaal bepaalt op basis van 1 enkele quote
std::string evaluateSignal(const OrderBook& book) {
    // 1. Basis berekeningen
    double spread = book.ask - book.bid;
    double mid = (book.bid + book.ask) / 2.0;

    // Veiligheidscheck om delen door nul te voorkomen
    double wmid = mid;

    //Formule voor de Gewogen midden.
    if ((book.bid_volume + book.ask_volume) > 0) {
        wmid = (book.bid_volume * book.ask + book.ask_volume * book.bid) / (book.bid_volume + book.ask_volume);
    }

    //Nu maken we de Ratio formule en die sluit perfect aan op Wpm.
    double Ratio;
    Ratio = (double)book.bid_volume / (book.bid_volume + book.ask_volume);

    // Print de basisdata
    std::cout << "Bid: " << book.bid << " | Ask: " << book.ask << " | Spread: " << spread << "\n";
    std::cout << "Mid: " << mid << " | Weighted Mid: " << wmid << "\n";

    // Bijhouden voor de meerderheids-beslissing
    spreadSum += spread;
    lastMid = mid;

    // 2. Signaal logica (Gebaseerd op Weighted Mid vs Mid)
    std::cout << "SIGNAL: ";
    if (wmid > mid && Ratio > 0.60) {
        std::cout << " BUY  (Kopersdruk dominant)\n";
        return "BUY";
    }
    else if (wmid < mid && Ratio < 0.40) {
        std::cout << " SELL  (Verkopersdruk dominant)\n";
        return "SELL";
    } else {
        std::cout << "NEUTRAAL (Volumes in balans)\n";
        return "NEUTRAAL";
    }
}

// Telt welk signaal het vaakst voorkwam in de buffer.
std::string majoritySignal(const std::vector<std::string>& signals) {
    std::map<std::string, int> counts;
    for (const auto& s : signals) counts[s]++;

    std::string best = "NEUTRAAL";
    int bestCount = -1;
    for (const auto& [sig, count] : counts) {
        if (count > bestCount) {
            bestCount = count;
            best = sig;
        }
    }

    std::cout << "== Telling laatste " << signals.size() << " signalen: "
              << "BUY=" << counts["BUY"]
              << " SELL=" << counts["SELL"]
              << " NEUTRAAL=" << counts["NEUTRAAL"]
              << " => Meerderheid: " << best << " ==\n";

    return best;
}

// Plaatst een bracket order (TP/SL) o.b.v. het meerderheidssignaal en de
// gemiddelde spread over het venster als volatiliteitsmaat.

void order(const std::string& signal) {
    // Check eerst of orders aan staan
    if (!orderyes) {
        std::cout << "orders zijn uitgeschakeld, geen orders geplaatst\n";
        return; // Breekt de functie af
    }

    // Code komt pas hier als orderyes true is, geen extra if meer nodig
    double avgSpread = spreadSum / TICKS_PER_DECISION;

    if (signal == "BUY") {
        double tp = lastMid + TP_SPREAD_MULTIPLIER * avgSpread;
        double sl = lastMid - SL_SPREAD_MULTIPLIER * avgSpread;
        std::cout << "-> BUY order: qty=1, TP=" << tp << " SL=" << sl
                  << " (avgSpread=" << avgSpread << ")\n";
        sendBracketOrder(symbol, "buy", "1", tp, sl);
    }
    else if (signal == "SELL") {
        double tp = lastMid - TP_SPREAD_MULTIPLIER * avgSpread;
        double sl = lastMid + SL_SPREAD_MULTIPLIER * avgSpread;
        std::cout << "-> SELL order: qty=1, TP=" << tp << " SL=" << sl
                  << " (avgSpread=" << avgSpread << ")\n";
        sendBracketOrder(symbol, "sell", "1", tp, sl);
    }
    else {
        std::cout << "Geen order. Meerderheid was NEUTRAAL\n";
    }
}

void startAlpacaFeed() {
    const char* key    = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    if (!key || !secret) {
        std::cerr << "ALPACA_API_KEY / ALPACA_API_SECRET niet gezet\n";
        return;
    }

    AlpacaWebSocket client(key, secret, {symbol});

    client.setQuoteCallback([](double bid, double ask,
                                double bid_volume, double ask_volume) {
        {
            std::lock_guard<std::mutex> lock(bookMutex);
            Apple_Book_Data.push_back(OrderBook{bid, ask, bid_volume, ask_volume});
            if (Apple_Book_Data.size() > 500) {
                Apple_Book_Data.pop_front();
            }
            newDataAvailable = true;
        }
        bookCv.notify_one();
    });

    client.run();
}

int main() {
    std::ios::sync_with_stdio(false);

    std::thread feed_thread(startAlpacaFeed);
    feed_thread.detach();

    while (true) {
        std::unique_lock<std::mutex> lock(bookMutex);
        bookCv.wait(lock, [] { return newDataAvailable; });
        newDataAvailable = false;

        if (!Apple_Book_Data.empty()) {
            OrderBook latestBook = Apple_Book_Data.back();
            lock.unlock();

            std::cout << "\n-- Nieuwe quote binnengekomen --\n";
            std::string signal = evaluateSignal(latestBook);
            signalBuffer.push_back(signal);

            if ((int)signalBuffer.size() >= TICKS_PER_DECISION) {
                std::string decision = majoritySignal(signalBuffer);
                order(decision);

                // Reset venster voor de volgende 60 ticks
                signalBuffer.clear();
                spreadSum = 0.0;
            }
        } else {
            lock.unlock();
        }
    }

    return 0;
}