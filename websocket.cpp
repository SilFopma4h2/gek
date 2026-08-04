#include "websocket.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

static const std::string ALPACA_HOST = "stream.data.alpaca.markets";
static const std::string ALPACA_PORT = "443";
static const std::string ALPACA_PATH = "/v2/iex";

// Reconnect-instellingen
static const int MAX_BACKOFF_SECONDS = 60;
static const int INITIAL_BACKOFF_SECONDS = 1;

AlpacaWebSocket::AlpacaWebSocket(std::string api_key,
                                 std::string api_secret,
                                 std::vector<std::string> symbols)
    : api_key_(std::move(api_key)),
      api_secret_(std::move(api_secret)),
      symbols_(std::move(symbols)) {}

void AlpacaWebSocket::setQuoteCallback(QuoteCallback cb) {
    callback_ = std::move(cb);
}

void AlpacaWebSocket::stop() {
    stop_requested_.store(true);
}

// Eén enkele connectiepoging. Retourneert wanneer de verbinding
// (normaal of door fout) eindigt.
bool AlpacaWebSocket::connectAndListen() {
    if (stop_requested_.load()) return false;

    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_default_verify_paths();

        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

        auto const results = resolver.resolve(ALPACA_HOST, ALPACA_PORT);
        net::connect(get_lowest_layer(ws), results);

        if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                       ALPACA_HOST.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()),
                                   net::error::get_ssl_category()));
        }

        ws.next_layer().handshake(ssl::stream_base::client);
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(beast::http::field::user_agent, "hft-orderbook-sim");
            }));
        ws.handshake(ALPACA_HOST, ALPACA_PATH);

        // Idle-timeout zodat een blokkerende read kan worden onderbroken voor
        // een nette shutdown en de verbinding nooit voor altijd blijft hangen.
        websocket::stream_base::timeout timeout_opt;
        timeout_opt.handshake_timeout = std::chrono::seconds(30);
        timeout_opt.idle_timeout = std::chrono::seconds(5);
        ws.set_option(timeout_opt);

        json auth_msg = {
            {"action", "auth"},
            {"key", api_key_},
            {"secret", api_secret_}
        };
        ws.write(net::buffer(auth_msg.dump()));

        // Wacht op het auth-resultaat vóór we subscriben, zodat foutieve
        // credentials direct zichtbaar zijn i.p.v. in een eindeloze retry-loop.
        beast::flat_buffer buffer;
        bool authenticated = false;
        while (ws.is_open() && !authenticated && !stop_requested_.load()) {
            buffer.clear();
            ws.read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());

            json parsed;
            try {
                parsed = json::parse(msg);
            } catch (...) {
                continue;
            }

            if (!parsed.is_array()) continue;

            for (auto& evt : parsed) {
                if (!evt.contains("T")) continue;
                std::string type = evt["T"];

                if (type == "error") {
                    std::cerr << "Alpaca error: " << evt.dump() << "\n";
                    return false;
                } else if (type == "success") {
                    std::cout << "Alpaca status: " << evt.dump() << "\n";
                    if (evt.value("msg", "").find("authenticated") != std::string::npos) {
                        authenticated = true;
                    }
                }
            }
        }

        if (!authenticated) {
            std::cerr << "Authenticatie bij Alpaca mislukt\n";
            return false;
        }

        json sub_msg = {
            {"action", "subscribe"},
            {"quotes", symbols_}
        };
        ws.write(net::buffer(sub_msg.dump()));

        while (ws.is_open() && !stop_requested_.load()) {
            buffer.clear();
            ws.read(buffer);
            std::string msg = beast::buffers_to_string(buffer.data());

            json parsed;
            try {
                parsed = json::parse(msg);
            } catch (...) {
                continue;
            }

            if (!parsed.is_array()) continue;

            for (auto& evt : parsed) {
                if (!evt.contains("T")) continue;
                std::string type = evt["T"];

                if (type == "q") {
                    double bp = evt.value("bp", 0.0);
                    double ap = evt.value("ap", 0.0);
                    double bs = evt.value("bs", 0.0);
                    double as = evt.value("as", 0.0);

                    if (callback_) {
                        callback_(bp, ap, bs, as);
                    }
                } else if (type == "error") {
                    std::cerr << "Alpaca error: " << evt.dump() << "\n";
                } else if (type == "success" || type == "subscription") {
                    std::cout << "Alpaca status: " << evt.dump() << "\n";
                }
            }
        }
        return true; // verbinding netjes gesloten
    } catch (std::exception const& e) {
        if (!stop_requested_.load()) {
            std::cerr << "WebSocket fout: " << e.what() << "\n";
        }
        return false; // fout, caller moet reconnecten
    }
}

void AlpacaWebSocket::run() {
    int backoff = INITIAL_BACKOFF_SECONDS;

    while (!stop_requested_.load()) {
        std::cout << "Verbinden met Alpaca websocket...\n";
        bool cleanExit = connectAndListen();

        if (stop_requested_.load()) break;

        if (cleanExit) {
            backoff = INITIAL_BACKOFF_SECONDS; // reset backoff na succesvolle sessie
        }

        std::cerr << "Verbinding verbroken, reconnect over " << backoff << "s...\n";
        // Sleep in stappen zodat stop() direct wordt opgepikt.
        for (int waited = 0; waited < backoff && !stop_requested_.load(); ++waited) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        backoff = std::min(backoff * 2, MAX_BACKOFF_SECONDS);
    }
}