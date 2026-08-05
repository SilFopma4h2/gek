// gui.cpp
//
// GUI version of Flow++. Same logic as main.cpp:
//   - OrderBook storage (deque, capped at 500)
//   - evaluateSignal() (weighted mid + ratio -> BUY/SELL/NEUTRAL)
//   - majoritySignal() over a time window (default 5 minutes)
//   - order() via Alpaca REST, TP/SL from average spread
//   - websocket feed via websocket.h/cpp (Alpaca IEX quotes)
//
// WSL2-proof: runs on WSLg (X11/Wayland) via GLFW + OpenGL, and
// prints a clear message if no display is around.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include "websocket.h"
#include "order.h"

// ---------------------------------------------------------------------------
// Config (same constants as main.cpp, now editable in the gui)
// ---------------------------------------------------------------------------
struct Settings {
    char symbol[16] = "SPY";
    bool orderEnabled = false;          // = orderyes in main.cpp
    float tpMult = 3.0f;                // = TP_SPREAD_MULTIPLIER
    float slMult = 1.5f;                // = SL_SPREAD_MULTIPLIER
    int intervalMinutes = 5;            // = DECISION_INTERVAL_SECONDS / 60
};
static Settings g_settings;

// ---------------------------------------------------------------------------
// Log (locked, the feed + order threads write here too)
// ---------------------------------------------------------------------------
struct LogEntry {
    std::string text;
    ImU32 color;
};

static std::mutex g_logMutex;
static std::deque<LogEntry> g_log;
static constexpr size_t LOG_CAP = 500;

static void logLine(const std::string& text, ImU32 color = IM_COL32(200, 200, 200, 255)) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_log.push_back({text, color});
    if (g_log.size() > LOG_CAP) g_log.pop_front();
}

static std::string nowStr() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
    return oss.str();
}

static std::string formatPrice(double p, int prec = 2) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(prec) << p;
    return oss.str();
}

static ImU32 signalColor(const std::string& s) {
    if (s == "BUY")    return IM_COL32(80, 220, 120, 255);
    if (s == "SELL")   return IM_COL32(235, 90, 90, 255);
    return IM_COL32(230, 190, 80, 255);
}

// ---------------------------------------------------------------------------
// Shared data between the feed thread and the GUI thread
// ---------------------------------------------------------------------------
struct OrderBook {
    double bid;
    double ask;
    double bid_volume;
    double ask_volume;
};

static std::mutex g_quoteMutex;
static std::deque<OrderBook> g_quoteQueue;   // incoming quotes from the feed

static std::mutex g_statusMutex;
static std::string g_statusText = "Not connected";
static bool g_connected = false;

static std::mutex g_feedMutex;
static std::shared_ptr<AlpacaWebSocket> g_client;
static std::thread g_feedThread;

// ---------------------------------------------------------------------------
// Trading state (gui thread only)
// ---------------------------------------------------------------------------
static std::deque<OrderBook> g_bookData;      // = symbolOrderBook (max 500)
static std::vector<std::string> g_signalBuffer;
static double g_spreadSum = 0.0;              // = spreadSum
static double g_lastMid = 0.0;                // = lastMid
static std::chrono::steady_clock::time_point g_windowStart =
    std::chrono::steady_clock::now();         // = windowStart

static std::deque<double> g_bidHistory;       // line chart data
static std::deque<double> g_askHistory;
static std::deque<double> g_midHistory;
static std::deque<double> g_spreadHistory;
static constexpr size_t CHART_CAP = 600;

struct DecisionRecord {
    std::string timeStr;
    std::string decision;
    int buy = 0, sell = 0, neu = 0;
    int ticks = 0;
    double entry = 0.0, tp = 0.0, sl = 0.0;
    bool orderPlaced = false;
};
static std::deque<DecisionRecord> g_decisions;
static constexpr size_t DECISION_CAP = 200;

static std::string g_lastSignal = "NEUTRAL";
static bool g_logEveryQuote = true;
static bool g_autoScroll = true;

// ---------------------------------------------------------------------------
// Signal logic (ported from main.cpp)
// ---------------------------------------------------------------------------
struct QuoteMetrics {
    double spread;
    double mid;
    double wmid;
    double ratio;
    std::string signal;
};

static QuoteMetrics computeMetrics(const OrderBook& book) {
    QuoteMetrics m;
    m.spread = book.ask - book.bid;
    m.mid = (book.bid + book.ask) / 2.0;

    // guard against div by zero
    m.wmid = m.mid;
    m.ratio = 0.0;
    if ((book.bid_volume + book.ask_volume) > 0) {
        m.wmid = (book.bid_volume * book.ask + book.ask_volume * book.bid)
                 / (book.bid_volume + book.ask_volume);
        m.ratio = (double)book.bid_volume / (book.bid_volume + book.ask_volume);
    }

    // weighted mid vs mid decides the signal
    if (m.wmid > m.mid && m.ratio > 0.60) {
        m.signal = "BUY";       // Buyer pressure dominant
    } else if (m.wmid < m.mid && m.ratio < 0.40) {
        m.signal = "SELL";      // Seller pressure dominant
    } else {
        m.signal = "NEUTRAL";   // Volumes balanced
    }
    return m;
}

// most common signal in the buffer (same as main.cpp)
static std::string majoritySignal(const std::vector<std::string>& signals,
                                  int& buy, int& sell, int& neu) {
    std::map<std::string, int> counts;
    for (const auto& s : signals) counts[s]++;

    buy = counts["BUY"];
    sell = counts["SELL"];
    neu = counts["NEUTRAL"];

    std::string best = "NEUTRAL";
    int bestCount = -1;
    for (const auto& [sig, count] : counts) {
        if (count > bestCount) {
            bestCount = count;
            best = sig;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Order planning and placement (ported from main.cpp: order())
// ---------------------------------------------------------------------------
struct OrderPlan {
    std::string side;
    double entry = 0.0;
    double tp = 0.0;
    double sl = 0.0;
    bool enabled = false;
};

static OrderPlan makeOrderPlan(const std::string& signal, int tickCount) {
    OrderPlan p;
    p.enabled = g_settings.orderEnabled;
    if (!p.enabled) return p;

    double avgSpread = tickCount > 0 ? g_spreadSum / tickCount : 0.0;
    double entry = g_lastMid;
    p.entry = entry;

    if (signal == "BUY") {
        p.side = "buy";
        p.tp = entry + g_settings.tpMult * avgSpread;
        p.sl = entry - g_settings.slMult * avgSpread;
    } else if (signal == "SELL") {
        p.side = "sell";
        p.tp = entry - g_settings.tpMult * avgSpread;
        p.sl = entry + g_settings.slMult * avgSpread;
    }
    return p;
}

static void dispatchOrderAsync(const OrderPlan& p, const std::string& signal) {
    logLine("-> " + signal + " limit+bracket: entry=" + formatPrice(p.entry)
            + " qty=1, TP=" + formatPrice(p.tp) + " SL=" + formatPrice(p.sl)
            + " (avgSpread=" + formatPrice(g_spreadSum / std::max(1, (int)g_signalBuffer.size()), 3) + ")",
            IM_COL32(120, 220, 120, 255));

    // fire the order on a worker thread so the ui stays responsive
    std::string symbol(g_settings.symbol);
    std::string side = p.side;
    double entry = p.entry, tp = p.tp, sl = p.sl;
    std::thread([symbol, side, entry, tp, sl]() {
        sendBracketOrder(symbol, side, "1", entry, tp, sl);
    }).detach();
}

// ---------------------------------------------------------------------------
// Decision (every intervalMinutes), mirrors the loop in main.cpp
// ---------------------------------------------------------------------------
static void runDecision() {
    int buy = 0, sell = 0, neu = 0;
    int ticks = (int)g_signalBuffer.size();
    std::string decision = "NEUTRAL";
    if (!g_signalBuffer.empty()) {
        decision = majoritySignal(g_signalBuffer, buy, sell, neu);
    }

    logLine("== Counting last " + std::to_string(ticks) + " signals: "
            + "BUY=" + std::to_string(buy)
            + " SELL=" + std::to_string(sell)
            + " NEUTRAL=" + std::to_string(neu)
            + " => Majority: " + decision + " ==", signalColor(decision));

    DecisionRecord rec;
    rec.timeStr = nowStr();
    rec.decision = decision;
    rec.buy = buy;
    rec.sell = sell;
    rec.neu = neu;
    rec.ticks = ticks;

    if (decision != "NEUTRAL") {
        OrderPlan plan = makeOrderPlan(decision, ticks);
        rec.entry = plan.entry;
        rec.tp = plan.tp;
        rec.sl = plan.sl;
        rec.orderPlaced = plan.enabled;
        if (plan.enabled) {
            dispatchOrderAsync(plan, decision);
        } else {
            logLine("Orders are disabled, no order placed.",
                    IM_COL32(180, 180, 180, 255));
        }
    } else {
        logLine("No order. Majority was NEUTRAL.", IM_COL32(180, 180, 180, 255));
    }

    g_decisions.push_front(rec);
    if (g_decisions.size() > DECISION_CAP) g_decisions.pop_back();

    // fresh window for the next interval
    g_signalBuffer.clear();
    g_spreadSum = 0.0;
    g_windowStart = std::chrono::steady_clock::now();
}

// decide as soon as the interval elapses, even with no fresh quotes
static void runDecisionCheck() {
    auto now = std::chrono::steady_clock::now();
    int intervalSec = g_settings.intervalMinutes * 60;
    if (std::chrono::duration_cast<std::chrono::seconds>(now - g_windowStart).count()
        >= intervalSec) {
        runDecision();
    }
}

// ---------------------------------------------------------------------------
// Feed on/off (runs the alpaca websocket on its own thread)
// ---------------------------------------------------------------------------
static bool feedIsRunning() {
    std::lock_guard<std::mutex> lock(g_feedMutex);
    return g_feedThread.joinable();
}

static void startFeed() {
    std::lock_guard<std::mutex> lock(g_feedMutex);
    if (g_feedThread.joinable()) {
        logLine("Feed is already running.", IM_COL32(230, 190, 80, 255));
        return;
    }

    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    if (!key || !secret) {
        logLine("ALPACA_API_KEY / ALPACA_API_SECRET not set.", IM_COL32(235, 90, 90, 255));
        {
            std::lock_guard<std::mutex> s(g_statusMutex);
            g_statusText = "No credentials";
            g_connected = false;
        }
        return;
    }

    std::string sym(g_settings.symbol);
    g_client = std::make_shared<AlpacaWebSocket>(key, secret,
                                                  std::vector<std::string>{sym});

    g_client->setQuoteCallback([](double bid, double ask,
                                  double bid_volume, double ask_volume) {
        std::lock_guard<std::mutex> lock(g_quoteMutex);
        g_quoteQueue.push_back(OrderBook{bid, ask, bid_volume, ask_volume});
    });

    g_client->setStatusCallback([](const std::string& msg) {
        logLine("[feed] " + msg, IM_COL32(150, 150, 255, 255));
        std::lock_guard<std::mutex> s(g_statusMutex);
        g_statusText = msg;
        if (msg.find("Authenticated") != std::string::npos) {
            g_connected = true;
        } else if (msg.find("lost") != std::string::npos ||
                   msg.find("error") != std::string::npos ||
                   msg.find("failed") != std::string::npos) {
            g_connected = false;
        }
    });

    g_feedThread = std::thread([](std::shared_ptr<AlpacaWebSocket> c) {
        c->run();
    }, g_client);

    logLine("Feed started for symbol " + sym, IM_COL32(80, 220, 120, 255));
}

static void stopFeed() {
    std::lock_guard<std::mutex> lock(g_feedMutex);
    if (!g_feedThread.joinable()) {
        logLine("Feed is not running.", IM_COL32(230, 190, 80, 255));
        return;
    }
    if (g_client) g_client->stop();
    g_feedThread.join();
    g_feedThread = std::thread();
    g_client.reset();
    {
        std::lock_guard<std::mutex> s(g_statusMutex);
        g_statusText = "Stopped";
        g_connected = false;
    }
    logLine("Feed stopped.", IM_COL32(180, 180, 180, 255));
}

// ---------------------------------------------------------------------------
// Drain incoming quotes (gui thread)
// ---------------------------------------------------------------------------
static void processQuotes() {
    std::deque<OrderBook> batch;
    {
        std::lock_guard<std::mutex> lock(g_quoteMutex);
        batch.swap(g_quoteQueue);
    }

    for (const auto& q : batch) {
        // orderbook storage, capped at 500 (same as main.cpp)
        g_bookData.push_back(q);
        if (g_bookData.size() > 500) g_bookData.pop_front();

        QuoteMetrics m = computeMetrics(q);
        g_spreadSum += m.spread;
        g_lastMid = m.mid;
        g_signalBuffer.push_back(m.signal);
        g_lastSignal = m.signal;

        g_midHistory.push_back(m.mid);
        g_spreadHistory.push_back(m.spread);
        g_bidHistory.push_back(q.bid);
        g_askHistory.push_back(q.ask);
        if (g_midHistory.size() > CHART_CAP) g_midHistory.pop_front();
        if (g_spreadHistory.size() > CHART_CAP) g_spreadHistory.pop_front();
        if (g_bidHistory.size() > CHART_CAP) g_bidHistory.pop_front();
        if (g_askHistory.size() > CHART_CAP) g_askHistory.pop_front();

        if (g_logEveryQuote) {
            logLine(nowStr() + " | Bid=" + formatPrice(q.bid)
                    + " Ask=" + formatPrice(q.ask)
                    + " Spread=" + formatPrice(m.spread, 3)
                    + " Mid=" + formatPrice(m.mid)
                    + " Wmid=" + formatPrice(m.wmid)
                    + " Ratio=" + formatPrice(m.ratio, 3)
                    + " => " + m.signal, signalColor(m.signal));
        }
    }
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------
static ImFont* g_bigFont = nullptr;

static void drawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        ImGui::Text("Flow++");
        ImGui::Separator();
        std::string status;
        bool connected;
        {
            std::lock_guard<std::mutex> s(g_statusMutex);
            status = g_statusText;
            connected = g_connected;
        }
        ImGui::TextColored(connected ? ImVec4(0.3f, 0.85f, 0.45f, 1.0f)
                                     : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                           "%s", connected ? "Connected" : "Offline");
        ImGui::Separator();
        ImGui::TextUnformatted(status.c_str());
        ImGui::EndMainMenuBar();
    }
}

static void drawConnectionPanel() {
    ImGui::SetNextWindowSize(ImVec2(380, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::Begin("Connection & Settings");

    ImGui::TextUnformatted("Alpaca credentials:");
    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    ImGui::BulletText("API key:    %s", (key && *key) ? "set" : "MISSING");
    ImGui::BulletText("API secret: %s", (secret && *secret) ? "set" : "MISSING");
    ImGui::Separator();

    bool running = feedIsRunning();
    ImGui::InputText("Symbol", g_settings.symbol, sizeof(g_settings.symbol));

    ImGui::BeginDisabled(running);
    if (ImGui::Button("Connect", ImVec2(120, 0))) startFeed();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Disconnect", ImVec2(120, 0))) stopFeed();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(running ? ImVec4(0.3f, 0.85f, 0.45f, 1.0f)
                               : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                       "%s", running ? (g_connected ? "connected" : "connecting...") : "off");

    std::string status;
    {
        std::lock_guard<std::mutex> s(g_statusMutex);
        status = g_statusText;
    }
    ImGui::TextUnformatted("Status:");
    ImGui::TextWrapped("%s", status.c_str());

    ImGui::Separator();
    ImGui::TextUnformatted("Settings (take effect from the next decision)");
    ImGui::Checkbox("Place orders (orderyes)", &g_settings.orderEnabled);
    ImGui::DragFloat("TP multiplier", &g_settings.tpMult, 0.1f, 0.5f, 10.0f);
    ImGui::DragFloat("SL multiplier", &g_settings.slMult, 0.1f, 0.5f, 10.0f);
    ImGui::DragInt("Interval (minutes)", &g_settings.intervalMinutes, 1, 1, 60);

    ImGui::Separator();
    if (ImGui::Button("Reset window", ImVec2(-1, 0))) {
        g_signalBuffer.clear();
        g_spreadSum = 0.0;
        g_windowStart = std::chrono::steady_clock::now();
        logLine("Window manually reset.", IM_COL32(180, 180, 180, 255));
    }

    ImGui::End();
}

static void drawChartPanel() {
    ImGui::SetNextWindowSize(ImVec2(860, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(400, 360), ImGuiCond_FirstUseEver);
    ImGui::Begin("Ticker Chart");

    if (g_midHistory.empty()) {
        ImGui::TextWrapped("No quotes received yet. Start the feed to see the ticker movement.");
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Legend:");
    ImGui::SameLine();
    ImGui::ColorButton("##bid_legend", ImVec4(0.30f, 0.55f, 0.95f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
    ImGui::SameLine();
    ImGui::TextUnformatted("Bid");
    ImGui::SameLine();
    ImGui::ColorButton("##ask_legend", ImVec4(0.95f, 0.40f, 0.35f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
    ImGui::SameLine();
    ImGui::TextUnformatted("Ask");
    ImGui::SameLine();
    ImGui::ColorButton("##mid_legend", ImVec4(0.95f, 0.85f, 0.25f, 1.0f), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
    ImGui::SameLine();
    ImGui::TextUnformatted("Mid");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu points)", g_midHistory.size());

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 pMin = ImGui::GetCursorScreenPos();
    ImVec2 pMax(pMin.x + avail.x, pMin.y + avail.y);
    if (avail.y > 16.0f) {
        ImGui::InvisibleButton("##chart", avail);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pMin, pMax, IM_COL32(20, 20, 24, 255));
        dl->AddRect(pMin, pMax, IM_COL32(70, 70, 80, 255));

        float lo = FLT_MAX, hi = -FLT_MAX;
        for (size_t i = 0; i < g_bidHistory.size(); ++i) {
            lo = std::min(lo, (float)g_bidHistory[i]);
            lo = std::min(lo, (float)g_askHistory[i]);
            lo = std::min(lo, (float)g_midHistory[i]);
            hi = std::max(hi, (float)g_bidHistory[i]);
            hi = std::max(hi, (float)g_askHistory[i]);
            hi = std::max(hi, (float)g_midHistory[i]);
        }
        float pad = (hi - lo) * 0.08f;
        if (pad < 1e-6f) { pad = 0.5f; }
        lo -= pad;
        hi += pad;
        const float range = (hi - lo) > 1e-9f ? (hi - lo) : 1.0f;

        // Grid lines + y-axis labels
        const int gridLines = 4;
        char buf[32];
        for (int i = 0; i <= gridLines; ++i) {
            float t = (float)i / (float)gridLines;
            float y = pMax.y - t * (pMax.y - pMin.y);
            dl->AddLine(ImVec2(pMin.x, y), ImVec2(pMax.x, y), IM_COL32(45, 45, 52, 255));
            float val = lo + t * range;
            snprintf(buf, sizeof(buf), "%.2f", val);
            dl->AddText(ImVec2(pMin.x + 4, y - 8), IM_COL32(160, 160, 170, 255), buf);
        }

        auto plotLine = [&](const std::deque<double>& data, ImU32 color) {
            if (data.empty()) return;
            int n = (int)data.size();
            std::vector<ImVec2> pts;
            pts.reserve(n);
            float x = pMax.x;
            float xStep = (pMax.x - pMin.x) / (float)(CHART_CAP - 1);
            for (int i = n - 1; i >= 0; --i) {
                float y = pMax.y - (float)((data[i] - lo) / range) * (pMax.y - pMin.y);
                pts.push_back(ImVec2(x, y));
                x -= xStep;
            }
            dl->AddPolyline(pts.data(), (int)pts.size(), color, 0, 2.0f);
        };

        plotLine(g_bidHistory, IM_COL32(77, 140, 242, 255));
        plotLine(g_askHistory, IM_COL32(242, 102, 89, 255));
        plotLine(g_midHistory, IM_COL32(242, 217, 64, 255));

        // Hover: show values at the mouse position
        if (ImGui::IsItemHovered()) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            ImGui::SetTooltip("Bid: %s\nAsk: %s\nMid: %s",
                              formatPrice(g_bidHistory.back()).c_str(),
                              formatPrice(g_askHistory.back()).c_str(),
                              formatPrice(g_midHistory.back()).c_str());
        }
    }

    ImGui::End();
}

static void drawMarketPanel() {
    ImGui::SetNextWindowSize(ImVec2(640, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(400, 30), ImGuiCond_FirstUseEver);
    ImGui::Begin("Market Data");

    if (g_bookData.empty()) {
        ImGui::TextWrapped("No quotes received yet. Start the feed to see live data.");
        ImGui::End();
        return;
    }

    const OrderBook& q = g_bookData.back();
    QuoteMetrics m = computeMetrics(q);

    ImGui::TextUnformatted("MID");
    if (g_bigFont) ImGui::PushFont(g_bigFont);
    ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.95f, 1.0f), "%s", formatPrice(m.mid).c_str());
    if (g_bigFont) ImGui::PopFont();

    ImGui::SameLine();
    ImGui::TextUnformatted("   Current signal:");
    ImGui::SameLine();
    if (g_bigFont) ImGui::PushFont(g_bigFont);
    ImVec4 sc = ImGui::ColorConvertU32ToFloat4(signalColor(g_lastSignal));
    ImGui::TextColored(sc, "%s", g_lastSignal.c_str());
    if (g_bigFont) ImGui::PopFont();

    ImGui::Separator();
    ImGui::Columns(2, "mkt", false);
    ImGui::Text("Bid:   %s  (vol %s)", formatPrice(q.bid).c_str(), formatPrice(q.bid_volume, 0).c_str());
    ImGui::Text("Ask:   %s  (vol %s)", formatPrice(q.ask).c_str(), formatPrice(q.ask_volume, 0).c_str());
    ImGui::Text("Spread: %s", formatPrice(m.spread, 3).c_str());
    ImGui::NextColumn();
    ImGui::Text("Weighted mid: %s", formatPrice(m.wmid).c_str());
    ImGui::Text("Bid-ratio:    %s", formatPrice(m.ratio, 3).c_str());
    ImGui::Text("Last update: %s", nowStr().c_str());
    ImGui::Columns(1);

    ImGui::ProgressBar((float)m.ratio, ImVec2(-1, 20), "bid-ratio");

    ImGui::Separator();
    if (!g_midHistory.empty()) {
        float lo = FLT_MAX, hi = -FLT_MAX;
        std::vector<float> mids(g_midHistory.begin(), g_midHistory.end());
        for (float v : mids) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (hi - lo < 1e-6) { lo -= 0.5f; hi += 0.5f; }
        ImGui::PlotLines("Mid price", mids.data(), (int)mids.size(),
                         0, nullptr, lo, hi, ImVec2(-1, 90));
    }

    ImGui::End();
}

static void drawSignalPanel() {
    ImGui::SetNextWindowSize(ImVec2(390, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1050, 30), ImGuiCond_FirstUseEver);
    ImGui::Begin("Signals");

    int buy = 0, sell = 0, neu = 0;
    for (const auto& s : g_signalBuffer) {
        if (s == "BUY") buy++;
        else if (s == "SELL") sell++;
        else neu++;
    }

    auto now = std::chrono::steady_clock::now();
    int intervalSec = g_settings.intervalMinutes * 60;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_windowStart).count();
    int remaining = (int)std::max(0LL, (long long)intervalSec - elapsed);

    ImGui::TextUnformatted("Window");
    ImGui::Text("Ticks: %zu", g_signalBuffer.size());
    ImGui::Text("Time to decision: %02d:%02d", remaining / 60, remaining % 60);

    ImGui::Separator();
    ImGui::TextUnformatted("Counts in this window:");
    ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.45f, 1.0f), "BUY      %d", buy);
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.0f), "SELL     %d", sell);
    ImGui::TextColored(ImVec4(0.9f, 0.75f, 0.35f, 1.0f), "NEUTRAL  %d", neu);

    float counts[3] = {(float)buy, (float)sell, (float)neu};
    ImGui::PlotHistogram("##counts", counts, 3, 0, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 60));

    ImGui::Separator();
    std::string decision = "NEUTRAL";
    int best = std::max({buy, sell, neu});
    if (best > 0) {
        if (buy == best) decision = "BUY";
        else if (sell == best) decision = "SELL";
        else decision = "NEUTRAL";
    }
    ImGui::TextUnformatted("Expected majority:");
    ImVec4 dc = ImGui::ColorConvertU32ToFloat4(signalColor(decision));
    ImGui::TextColored(dc, "%s", decision.c_str());

    ImGui::Separator();
    if (ImGui::Button("Decide now", ImVec2(-1, 0))) {
        runDecision();
    }

    ImGui::End();
}

static void drawDecisionPanel() {
    ImGui::SetNextWindowSize(ImVec2(760, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(10, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Decisions");

    if (ImGui::BeginTable("dec", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
                                    ImGuiTableFlags_BordersH | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Decision");
        ImGui::TableSetupColumn("BUY");
        ImGui::TableSetupColumn("SELL");
        ImGui::TableSetupColumn("NEU");
        ImGui::TableSetupColumn("Entry");
        ImGui::TableSetupColumn("TP");
        ImGui::TableSetupColumn("SL");
        ImGui::TableSetupColumn("Order");
        ImGui::TableHeadersRow();

        for (auto it = g_decisions.begin(); it != g_decisions.end(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it->timeStr.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(signalColor(it->decision)), "%s", it->decision.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", it->buy);
            ImGui::TableNextColumn(); ImGui::Text("%d", it->sell);
            ImGui::TableNextColumn(); ImGui::Text("%d", it->neu);
            ImGui::TableNextColumn(); ImGui::Text("%s", formatPrice(it->entry).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", formatPrice(it->tp).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", formatPrice(it->sl).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(
                it->orderPlaced ? ImVec4(0.3f, 0.85f, 0.45f, 1.0f)
                                : ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "%s", it->orderPlaced ? "placed" : "-");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

static void drawLogPanel() {
    ImGui::SetNextWindowSize(ImVec2(680, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(780, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log");

    ImGui::Checkbox("Log every quote", &g_logEveryQuote);
    ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &g_autoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.clear();
    }

    ImGui::Separator();
    if (ImGui::BeginChild("##logscroll", ImVec2(0, 0), false,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        std::vector<LogEntry> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            snapshot.assign(g_log.begin(), g_log.end());
        }
        for (const auto& e : snapshot) {
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(e.color), "%s", e.text.c_str());
        }
        if (g_autoScroll) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// WSL2-proof window setup
// ---------------------------------------------------------------------------
static void glfwErrorCallback(int code, const char* desc) {
    std::cerr << "GLFW error (" << code << "): " << (desc ? desc : "?") << "\n";
}

int main() {
    std::ios::sync_with_stdio(false);
    logLine("Flow++ GUI started. Close the window to stop.",
            IM_COL32(200, 200, 200, 255));

    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW.\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(1500, 900, "Flow++ - Alpaca trading GUI",
                                          nullptr, nullptr);
    if (!window) {
        const char* desc = nullptr;
        int code = glfwGetError(&desc);
        std::cerr << "Failed to create window (GLFW " << code << ": "
                  << (desc ? desc : "?") << ").\n"
                  << "WSL2: make sure WSLg is active or an X server is running "
                  << "(check 'echo $DISPLAY').\n";
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "flow_gui.ini";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.WindowBorderSize = 1.0f;

    ImFontConfig cfg;
    cfg.SizePixels = 15.0f;
    io.Fonts->AddFontDefault(&cfg);
    ImFontConfig cfgBig;
    cfgBig.SizePixels = 34.0f;
    g_bigFont = io.Fonts->AddFontDefault(&cfgBig);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // route order results to the gui log instead of stdout
    setOrderLogCallback([](const std::string& msg, bool isError) {
        logLine(msg, isError ? IM_COL32(235, 90, 90, 255)
                             : IM_COL32(80, 220, 120, 255));
    });

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        processQuotes();
        runDecisionCheck();

        drawMenuBar();
        drawConnectionPanel();
        drawMarketPanel();
        drawChartPanel();
        drawSignalPanel();
        drawDecisionPanel();
        drawLogPanel();

        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    stopFeed();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    std::cout << "Flow++ GUI stopped.\n";
    return 0;
}
