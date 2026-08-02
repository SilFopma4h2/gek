#include <iostream>
#include <cmath>
#include <deque>
#include "websocket.h"
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include "order.h"
//Hier de confg
const std::string symbol = "SPY";

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

    // 2. Signaal logica (Gebaseerd op Weighted Mid vs Mid)
    // Als Wmid > Mid: er staat meer volume aan de bid kant (kopersdruk) -> KOOP signaal
    // Als Wmid < Mid: er staat meer volume aan de ask kant (verkopersdruk) -> VERKOOP signaal
    std::cout << "SIGNAL: ";
    if (wmid > mid && Ratio > 0.60) {
    //Voor een beter signaal pakken we de Ratio er ook bij.
        std::cout << " BUY  (Kopersdruk dominant)\n";
        return "BUY";
    }
    //Else if functie word aangeroepen als ALLEEN als return "BUY" false is dus als wmid < mid dan worden de volgende statements gepakt.
    else if (wmid < mid && Ratio < 0.40) {
        std::cout << " SELL  (Verkopersdruk dominant)\n";
        return "SELL";

    } else {
        std::cout << "NEUTRAAL (Volumes in balans)\n";
        return "NEUTRAAL";
    }




}

//Blokje voor de orders
void order(std::string& signal) {
    if (signal == "BUY") {
        sendOrder(symbol, "buy", "0.01");
    }
    else if (signal == "SELL") {
        sendOrder(symbol, "sell", "0.01");
    }
    else if (signal == "NEUTRAAL") {
        std::cout << "Geen order. Markt op break even\n";
    }
    else {
        std::cout << "Oei een error";
    }


}


void startAlpacaFeed() {
    const char* key    = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    if (!key || !secret) {
        std::cerr << "ALPACA_API_KEY / ALPACA_API_SECRET niet gezet\n";
        return;
    }

    //Ticker stt=aat bovenaan.
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

        // Pak alleen de laatste quote uit de vector
        if (!Apple_Book_Data.empty()) {
            OrderBook latestBook = Apple_Book_Data.back();

            // Ontgrendel de mutex voordat we gaan printen/rekenen
            // zodat de WebSocket thread niet geblokkeerd wordt
            lock.unlock();

            std::cout << "\n-- Nieuwe quote binnengekomen --\n";
            evaluateSignal(latestBook);
        } else {
            lock.unlock();
        }
    }

return 0;
}