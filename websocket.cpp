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

// Reconnect settings
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

void AlpacaWebSocket::setStatusCallback(StatusCallback cb) {
    status_cb_ = std::move(cb);
}

void AlpacaWebSocket::stop() {
    stop_requested_.store(true);
}

void AlpacaWebSocket::notifyStatus(const std::string& msg) const {
    if (status_cb_) status_cb_(msg);
}

// One connection attempt; returns when the connection closes.
bool AlpacaWebSocket::connectAndListen() {
    if (stop_requested_.load()) return false;

    notifyStatus("Connecting to " + ALPACA_HOST + "...");

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
        notifyStatus("WebSocket connected, authenticating...");

        // Idle timeout so a blocking read can't hang the shutdown forever.
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

        // Wait for auth before subscribing, so bad keys fail fast instead of
        // retrying forever.
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
                    notifyStatus("Alpaca error: " + evt.dump());
                    return false;
                } else if (type == "success") {
                    std::cout << "Alpaca status: " << evt.dump() << "\n";
                    notifyStatus("Alpaca: " + evt.dump());
                    if (evt.value("msg", "").find("authenticated") != std::string::npos) {
                        authenticated = true;
                    }
                }
            }
        }

        if (!authenticated) {
            std::cerr << "Alpaca authentication failed\n";
            notifyStatus("Authentication failed");
            return false;
        }

        notifyStatus("Authenticated, subscribing to: " + [this]() {
            std::string s;
            for (size_t i = 0; i < symbols_.size(); ++i) {
                if (i) s += ", ";
                s += symbols_[i];
            }
            return s;
        }());

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
                    notifyStatus("Alpaca error: " + evt.dump());
                } else if (type == "success" || type == "subscription") {
                    std::cout << "Alpaca status: " << evt.dump() << "\n";
                    notifyStatus("Alpaca: " + evt.dump());
                }
            }
        }
        return true; // connection closed cleanly
    } catch (std::exception const& e) {
        if (!stop_requested_.load()) {
            std::cerr << "WebSocket error: " << e.what() << "\n";
            notifyStatus("WebSocket error: " + std::string(e.what()));
        }
        return false; // error, caller should reconnect
    }
}

void AlpacaWebSocket::run() {
    int backoff = INITIAL_BACKOFF_SECONDS;

    while (!stop_requested_.load()) {
        std::cout << "Connecting to Alpaca websocket...\n";
        notifyStatus("Reconnecting in " + std::to_string(backoff) + "s...");
        bool cleanExit = connectAndListen();

        if (stop_requested_.load()) break;

        if (cleanExit) {
            backoff = INITIAL_BACKOFF_SECONDS; // reset backoff after a successful session
        }

        std::cerr << "Connection lost, reconnecting in " << backoff << "s...\n";
        notifyStatus("Connection lost");
        // Sleep in steps so stop() is picked up quickly.
        for (int waited = 0; waited < backoff && !stop_requested_.load(); ++waited) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        backoff = std::min(backoff * 2, MAX_BACKOFF_SECONDS);
    }
}
