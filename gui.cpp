// gui.cpp
//
// GUI version of Flow++. Same logic as main.cpp:
//   - OrderBook storage (deque, capped at 500)
//   - evaluateSignal() (weighted mid + ratio -> BUY/SELL/NEUTRAL)
//   - majoritySignal() over a time window (default 5 minutes)
//   - order() via Alpaca REST, TP/SL from average spread
//   - websocket feed via websocket.h/cpp (Alpaca IEX quotes)
//
// Runs on WSLg (X11/Wayland) via GLFW + OpenGL; prints a clear
// message if there's no display around.

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
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
// palette + theme
// ---------------------------------------------------------------------------
namespace col {
const ImU32 bg       = IM_COL32(20, 22, 26, 255);
const ImU32 panel    = IM_COL32(24, 27, 32, 255);
const ImU32 panelLt  = IM_COL32(29, 33, 39, 255);
const ImU32 border   = IM_COL32(48, 54, 63, 255);
const ImU32 text     = IM_COL32(230, 237, 243, 255);
const ImU32 textDim  = IM_COL32(139, 148, 158, 255);
const ImU32 accent   = IM_COL32(88, 166, 255, 255);
const ImU32 blue     = IM_COL32(88, 166, 255, 255);
const ImU32 green    = IM_COL32(63, 185, 80, 255);
const ImU32 red      = IM_COL32(248, 81, 73, 255);
const ImU32 yellow   = IM_COL32(210, 153, 34, 255);
}

static void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding    = ImVec2(10, 10);
    s.FramePadding     = ImVec2(6, 4);
    s.ItemSpacing      = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.WindowRounding   = 6.0f;
    s.ChildRounding    = 4.0f;
    s.FrameRounding    = 4.0f;
    s.PopupRounding    = 4.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding     = 4.0f;
    s.TabRounding      = 4.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.ScrollbarSize    = 10.0f;
    s.GrabMinSize      = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.902f, 0.929f, 0.953f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.545f, 0.580f, 0.620f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.094f, 0.106f, 0.125f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.078f, 0.086f, 0.102f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.094f, 0.106f, 0.125f, 0.98f);
    c[ImGuiCol_Border]               = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.114f, 0.129f, 0.153f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.153f, 0.173f, 0.204f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.153f, 0.173f, 0.204f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.094f, 0.106f, 0.125f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.114f, 0.129f, 0.153f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.094f, 0.106f, 0.125f, 0.60f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.094f, 0.106f, 0.125f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.078f, 0.086f, 0.102f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.298f, 0.333f, 0.384f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.345f, 0.384f, 0.443f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.345f, 0.651f, 1.000f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.345f, 0.651f, 1.000f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.510f, 0.720f, 1.000f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.114f, 0.129f, 0.153f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.247f, 0.447f, 0.702f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.247f, 0.447f, 0.702f, 0.40f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.247f, 0.447f, 0.702f, 0.60f);
    c[ImGuiCol_Separator]            = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.345f, 0.651f, 1.000f, 0.60f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.345f, 0.651f, 1.000f, 0.80f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.345f, 0.651f, 1.000f, 0.20f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.345f, 0.651f, 1.000f, 0.50f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.345f, 0.651f, 1.000f, 0.70f);
    c[ImGuiCol_Tab]                  = ImVec4(0.114f, 0.129f, 0.153f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.247f, 0.447f, 0.702f, 0.50f);
    c[ImGuiCol_TabActive]            = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.094f, 0.106f, 0.125f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.114f, 0.129f, 0.153f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.114f, 0.129f, 0.153f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.188f, 0.212f, 0.247f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.153f, 0.173f, 0.204f, 1.00f);
    c[ImGuiCol_PlotLines]            = ImVec4(0.345f, 0.651f, 1.000f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.510f, 0.720f, 1.000f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.345f, 0.651f, 1.000f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.510f, 0.720f, 1.000f, 1.00f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.247f, 0.447f, 0.702f, 0.40f);
}

// ---------------------------------------------------------------------------
// config (same constants as main.cpp, editable here)
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
// log (locked; feed + order threads write here)
// ---------------------------------------------------------------------------
struct LogEntry {
    std::string text;
    ImU32 color;
};

static std::mutex g_logMutex;
static std::deque<LogEntry> g_log;
static constexpr size_t LOG_CAP = 500;

static void logLine(const std::string& text, ImU32 color = col::textDim) {
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
    if (s == "BUY")  return col::green;
    if (s == "SELL") return col::red;
    return col::yellow;
}

static ImVec4 colF(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// ---------------------------------------------------------------------------
// shared data: feed thread <-> gui thread
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
// trading state (gui thread only)
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
// signal logic (ported from main.cpp)
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
        m.signal = "BUY";       // buyer pressure
    } else if (m.wmid < m.mid && m.ratio < 0.40) {
        m.signal = "SELL";      // seller pressure
    } else {
        m.signal = "NEUTRAL";   // volumes balanced
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
// order planning + placement (ported from main.cpp)
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
            col::green);

    // fire the order on a worker thread so the ui stays responsive
    std::string symbol(g_settings.symbol);
    std::string side = p.side;
    double entry = p.entry, tp = p.tp, sl = p.sl;
    std::thread([symbol, side, entry, tp, sl]() {
        sendBracketOrder(symbol, side, "1", entry, tp, sl);
    }).detach();
}

// ---------------------------------------------------------------------------
// decision (every intervalMinutes), same loop as main.cpp
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
            logLine("Orders are disabled, no order placed.", col::textDim);
        }
    } else {
        logLine("No order. Majority was NEUTRAL.", col::textDim);
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
// feed on/off (websocket on its own thread)
// ---------------------------------------------------------------------------
static bool feedIsRunning() {
    std::lock_guard<std::mutex> lock(g_feedMutex);
    return g_feedThread.joinable();
}

static void startFeed() {
    std::lock_guard<std::mutex> lock(g_feedMutex);
    if (g_feedThread.joinable()) {
        logLine("Feed is already running.", col::yellow);
        return;
    }

    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    if (!key || !secret) {
        logLine("ALPACA_API_KEY / ALPACA_API_SECRET not set.", col::red);
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
        logLine("[feed] " + msg, col::accent);
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

    logLine("Feed started for symbol " + sym, col::green);
}

static void stopFeed() {
    std::lock_guard<std::mutex> lock(g_feedMutex);
    if (!g_feedThread.joinable()) {
        logLine("Feed is not running.", col::yellow);
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
    logLine("Feed stopped.", col::textDim);
}

// ---------------------------------------------------------------------------
// drain incoming quotes (gui thread)
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
// layout: fixed grid, windows still draggable/resizable
// ---------------------------------------------------------------------------
struct Layout {
    ImVec2 connPos, connSz;
    ImVec2 marketPos, marketSz;
    ImVec2 signalsPos, signalsSz;
    ImVec2 chartPos, chartSz;
    ImVec2 logPos, logSz;
    ImVec2 decPos, decSz;
};

static Layout layout() {
    const ImVec2 d = ImGui::GetIO().DisplaySize;
    const float m = 8.0f, gap = 8.0f, top = 28.0f;
    const float w = d.x - 2.0f * m;
    const float h = std::max(300.0f, d.y - top - m);
    const float leftW = 360.0f;
    const float rightW = 380.0f;
    const float midW = std::max(240.0f, w - leftW - rightW - 2.0f * gap);
    const float bottomH = 200.0f;
    const float upperH = std::max(140.0f, h - bottomH - gap);
    const float halfH = (upperH - gap) * 0.5f;

    const float midX = m + leftW + gap;
    const float rightX = midX + midW + gap;

    Layout L;
    L.connPos = ImVec2(m, top);
    L.connSz  = ImVec2(leftW, upperH);

    L.marketPos = ImVec2(midX, top);
    L.marketSz  = ImVec2(midW, halfH);

    L.signalsPos = ImVec2(rightX, top);
    L.signalsSz  = ImVec2(rightW, halfH);

    L.chartPos = ImVec2(midX, top + halfH + gap);
    L.chartSz  = ImVec2(midW, halfH);

    L.logPos = ImVec2(rightX, top + halfH + gap);
    L.logSz  = ImVec2(rightW, halfH);

    L.decPos = ImVec2(m, top + upperH + gap);
    L.decSz  = ImVec2(w, bottomH);
    return L;
}

static ImVec4 connectedColor(bool running) {
    if (running) return g_connected ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f)
                                    : ImVec4(0.824f, 0.600f, 0.133f, 1.0f);
    return ImVec4(0.545f, 0.580f, 0.620f, 1.0f);
}

// force the grid once on startup so old flow_gui.ini
// positions don't overlap the windows
static bool g_forceLayout = true;

static void applyPanelPlacement(const ImVec2& pos, const ImVec2& sz) {
    ImGuiCond cond = g_forceLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(pos, cond);
    ImGui::SetNextWindowSize(sz, cond);
}

static void centerHint(const char* text) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((avail.x - textSize.x) * 0.5f,
                               (avail.y - textSize.y) * 0.5f));
    ImGui::TextDisabled("%s", text);
}

static ImFont* g_bigFont = nullptr;

// ---------------------------------------------------------------------------
// menu bar
// ---------------------------------------------------------------------------
static void drawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextColored(ImVec4(0.345f, 0.651f, 1.0f, 1.0f), "Flow++");
        ImGui::Separator();

        std::string status;
        bool connected;
        {
            std::lock_guard<std::mutex> s(g_statusMutex);
            status = g_statusText;
            connected = g_connected;
        }

        ImGui::Text("Symbol: %s", g_settings.symbol);
        ImGui::Separator();

        ImU32 dotCol = connected ? col::green : col::red;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(pos.x + 4, pos.y + 8), 4.0f, dotCol);
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 12, pos.y));
        ImGui::TextUnformatted(connected ? "Connected" : "Offline");
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", status.c_str());
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::Text("Signal: ");
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(signalColor(g_lastSignal)),
                           "%s", g_lastSignal.c_str());

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset layout")) {
                const char* fname = ImGui::GetIO().IniFilename;
                if (fname) std::remove(fname);
                ImGui::LoadIniSettingsFromMemory("", 0);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

// ---------------------------------------------------------------------------
// connection & settings
// ---------------------------------------------------------------------------
static void drawConnectionPanel() {
    Layout L = layout();
    applyPanelPlacement(L.connPos, L.connSz);
    ImGui::Begin("Connection & Settings");

    ImGui::SeparatorText("Credentials");
    const char* key = std::getenv("ALPACA_API_KEY");
    const char* secret = std::getenv("ALPACA_API_SECRET");
    bool keyOk = key && *key;
    bool secOk = secret && *secret;
    ImGui::TextColored(keyOk ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f)
                             : ImVec4(0.973f, 0.318f, 0.286f, 1.0f),
                       "[%s] API key", keyOk ? "ok" : "missing");
    ImGui::TextColored(secOk ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f)
                             : ImVec4(0.973f, 0.318f, 0.286f, 1.0f),
                       "[%s] API secret", secOk ? "ok" : "missing");

    bool running = feedIsRunning();
    ImGui::InputText("Symbol", g_settings.symbol, sizeof(g_settings.symbol),
                     ImGuiInputTextFlags_CharsUppercase);

    ImGui::BeginDisabled(running);
    bool connect = ImGui::Button("Connect", ImVec2(-1, 0));
    ImGui::EndDisabled();
    if (connect) startFeed();

    ImGui::BeginDisabled(!running);
    bool disconnect = ImGui::Button("Disconnect", ImVec2(-1, 0));
    ImGui::EndDisabled();
    if (disconnect) stopFeed();

    {
        std::lock_guard<std::mutex> s(g_statusMutex);
        ImGui::TextColored(connectedColor(running), "%s",
                           running ? (g_connected ? "connected" : "connecting...") : "off");
    }
    ImGui::SeparatorText("Status");
    {
        std::lock_guard<std::mutex> s(g_statusMutex);
        ImGui::TextWrapped("%s", g_statusText.c_str());
    }

    ImGui::SeparatorText("Trading");
    ImGui::TextDisabled("Settings apply from the next decision");
    ImGui::Checkbox("Place orders", &g_settings.orderEnabled);
    ImGui::DragFloat("TP multiplier", &g_settings.tpMult, 0.1f, 0.5f, 10.0f, "%.2f");
    ImGui::DragFloat("SL multiplier", &g_settings.slMult, 0.1f, 0.5f, 10.0f, "%.2f");

    ImGui::SeparatorText("Window");
    ImGui::DragInt("Interval (minutes)", &g_settings.intervalMinutes, 1, 1, 60);
    if (ImGui::Button("Reset window", ImVec2(-1, 0))) {
        g_signalBuffer.clear();
        g_spreadSum = 0.0;
        g_windowStart = std::chrono::steady_clock::now();
        logLine("Window manually reset.", col::textDim);
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// ticker chart (custom draw)
// ---------------------------------------------------------------------------
static void drawChartPanel() {
    Layout L = layout();
    applyPanelPlacement(L.chartPos, L.chartSz);
    ImGui::Begin("Ticker Chart");
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (g_midHistory.empty()) {
        centerHint("Start the feed to see the ticker movement.");
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.345f, 0.651f, 1.0f, 1.0f), "Bid");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.973f, 0.318f, 0.286f, 1.0f), "Ask");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.824f, 0.600f, 0.133f, 1.0f), "Mid");
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu pts)", g_midHistory.size());

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 pMin = ImGui::GetCursorScreenPos();
    ImVec2 pMax(pMin.x + avail.x, pMin.y + avail.y);
    if (avail.y <= 16.0f) { ImGui::End(); return; }

    ImGui::InvisibleButton("##chart", avail);
    bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(pMin, pMax, col::bg);
    dl->AddRect(pMin, pMax, col::border);

    float lo = FLT_MAX, hi = -FLT_MAX;
    for (size_t i = 0; i < g_midHistory.size(); ++i) {
        lo = std::min({lo, (float)g_bidHistory[i], (float)g_askHistory[i], (float)g_midHistory[i]});
        hi = std::max({hi, (float)g_bidHistory[i], (float)g_askHistory[i], (float)g_midHistory[i]});
    }
    float pad = (hi - lo) * 0.06f;
    if (pad < 1e-6f) pad = 0.5f;
    lo -= pad;
    hi += pad;
    const float range = std::max(hi - lo, 1e-6f);

    const float labelW = 48.0f;
    const float plotL = pMin.x + labelW;
    const int gridLines = 4;
    char buf[32];
    for (int i = 0; i <= gridLines; ++i) {
        float t = (float)i / (float)gridLines;
        float y = pMax.y - t * (pMax.y - pMin.y);
        dl->AddLine(ImVec2(plotL, y), ImVec2(pMax.x, y), IM_COL32(45, 45, 55, 255));
        float val = lo + t * range;
        snprintf(buf, sizeof(buf), "%.2f", val);
        dl->AddText(ImVec2(pMin.x + 4, y - 8), col::textDim, buf);
    }

    auto plot = [&](const std::deque<double>& data, ImU32 c) {
        int n = (int)data.size();
        if (n == 0) return;
        if (n == 1) {
            float y = pMax.y - (float)((data[0] - lo) / range) * (pMax.y - pMin.y);
            dl->AddCircleFilled(ImVec2(plotL, y), 3.0f, c);
            return;
        }
        std::vector<ImVec2> pts;
        pts.reserve(n);
        float step = (pMax.x - plotL) / (float)(n - 1);
        for (int i = 0; i < n; ++i) {
            float x = plotL + step * (float)i;
            float y = pMax.y - (float)((data[i] - lo) / range) * (pMax.y - pMin.y);
            pts.push_back(ImVec2(x, y));
        }
        dl->AddPolyline(pts.data(), (int)pts.size(), c, 0, 1.6f);
    };
    plot(g_bidHistory, col::blue);
    plot(g_askHistory, col::red);
    plot(g_midHistory, col::yellow);

    if (hovered) {
        int n = (int)g_midHistory.size();
        float mx = ImGui::GetIO().MousePos.x;
        int idx;
        if (n == 1) {
            idx = 0;
        } else {
            float step = (pMax.x - plotL) / (float)(n - 1);
            idx = (int)std::round((mx - plotL) / step);
            idx = std::clamp(idx, 0, n - 1);
        }
        float hx = (n == 1) ? plotL : plotL + ((pMax.x - plotL) / (float)(n - 1)) * idx;
        dl->AddLine(ImVec2(hx, pMin.y), ImVec2(hx, pMax.y), col::border);
        float my = pMax.y - (float)((g_midHistory[idx] - lo) / range) * (pMax.y - pMin.y);
        dl->AddCircleFilled(ImVec2(hx, my), 3.0f, col::yellow);

        ImGui::SetTooltip("Bid: %s\nAsk: %s\nMid: %s\nSpread: %s",
                          formatPrice(g_bidHistory[idx]).c_str(),
                          formatPrice(g_askHistory[idx]).c_str(),
                          formatPrice(g_midHistory[idx]).c_str(),
                          formatPrice(g_askHistory[idx] - g_bidHistory[idx], 3).c_str());
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// market data
// ---------------------------------------------------------------------------
static void drawMarketPanel() {
    Layout L = layout();
    applyPanelPlacement(L.marketPos, L.marketSz);
    ImGui::Begin("Market Data");

    if (g_bookData.empty()) {
        centerHint("Start the feed to see live data.");
        ImGui::End();
        return;
    }

    const OrderBook& q = g_bookData.back();
    QuoteMetrics m = computeMetrics(q);

    ImGui::TextUnformatted("MID");
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(signalColor(g_lastSignal)),
                       "[%s]", g_lastSignal.c_str());
    if (g_bigFont) ImGui::PushFont(g_bigFont);
    ImGui::TextColored(ImVec4(0.902f, 0.929f, 0.953f, 1.0f), "%s",
                       formatPrice(m.mid).c_str());
    if (g_bigFont) ImGui::PopFont();

    ImGui::SeparatorText("Order book");
    if (ImGui::BeginTable("book", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("left", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::Text("Bid");      ImGui::SameLine();
        ImGui::TextColored(colF(col::blue), "%s", formatPrice(q.bid).c_str());
        ImGui::Text("Vol");      ImGui::SameLine();
        ImGui::TextDisabled("%s", formatPrice(q.bid_volume, 0).c_str());

        ImGui::TableNextColumn();
        ImGui::Text("Ask");      ImGui::SameLine();
        ImGui::TextColored(colF(col::red), "%s", formatPrice(q.ask).c_str());
        ImGui::Text("Vol");      ImGui::SameLine();
        ImGui::TextDisabled("%s", formatPrice(q.ask_volume, 0).c_str());
        ImGui::EndTable();
    }

    ImGui::Text("Spread");      ImGui::SameLine();
    ImGui::TextColored(colF(col::yellow), "%s", formatPrice(m.spread, 3).c_str());
    ImGui::Text("Weighted mid"); ImGui::SameLine();
    ImGui::TextColored(colF(col::accent), "%s", formatPrice(m.wmid).c_str());

    ImGui::SeparatorText("Bid ratio");
    ImGui::ProgressBar((float)m.ratio, ImVec2(-1, 18),
                       formatPrice(m.ratio, 3).c_str());
    ImGui::TextDisabled("Last update: %s", nowStr().c_str());

    ImGui::SeparatorText("Mid price");
    if (!g_midHistory.empty()) {
        float lo = FLT_MAX, hi = -FLT_MAX;
        std::vector<float> mids(g_midHistory.begin(), g_midHistory.end());
        for (float v : mids) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        if (hi - lo < 1e-6) { lo -= 0.5f; hi += 0.5f; }
        ImGui::PlotLines("##mid", mids.data(), (int)mids.size(),
                         0, nullptr, lo, hi, ImVec2(-1, 60));
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// signals
// ---------------------------------------------------------------------------
static void drawSignalPanel() {
    Layout L = layout();
    applyPanelPlacement(L.signalsPos, L.signalsSz);
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

    ImGui::Text("Ticks: %zu", g_signalBuffer.size());
    ImGui::Text("Decision in: %02d:%02d", remaining / 60, remaining % 60);

    ImGui::SeparatorText("Counts this window");
    ImGui::TextColored(colF(col::green), "BUY     %d", buy);
    ImGui::TextColored(colF(col::red),   "SELL    %d", sell);
    ImGui::TextColored(colF(col::yellow), "NEUTRAL %d", neu);

    // colored bar chart
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float bw = ImGui::GetContentRegionAvail().x;
    const float barH = 54.0f;
    const float gap = 6.0f;
    const float w = (bw - 2.0f * gap) / 3.0f;
    int maxCount = std::max({buy, sell, neu, 1});
    auto bar = [&](float x, ImU32 c, int val) {
        float h = barH * ((float)val / (float)maxCount);
        dl->AddRectFilled(ImVec2(a.x + x, a.y + barH - h),
                          ImVec2(a.x + x + w, a.y + barH), c);
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%d", val);
        dl->AddText(ImVec2(a.x + x + w * 0.5f - 6.0f, a.y + barH - h - 16.0f),
                    col::text, lbl);
    };
    bar(0.0f, col::green, buy);
    bar(w + gap, col::red, sell);
    bar(2.0f * (w + gap), col::yellow, neu);
    ImGui::Dummy(ImVec2(bw, barH + 18.0f));

    ImGui::SeparatorText("Expected majority");
    std::string decision = "NEUTRAL";
    int best = std::max({buy, sell, neu});
    if (best > 0) {
        if (buy == best) decision = "BUY";
        else if (sell == best) decision = "SELL";
        else decision = "NEUTRAL";
    }
    ImGui::TextColored(colF(signalColor(decision)), "%s", decision.c_str());

    ImGui::Separator();
    if (ImGui::Button("Decide now", ImVec2(-1, 0))) {
        runDecision();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// decisions
// ---------------------------------------------------------------------------
static void drawDecisionPanel() {
    Layout L = layout();
    applyPanelPlacement(L.decPos, L.decSz);
    ImGui::Begin("Decisions");

    if (g_decisions.empty()) {
        centerHint("No decisions yet.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable("dec", 9,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
                          ImGuiTableFlags_BordersH | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Time",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Decision", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("BUY",     ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("SELL",    ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("NEU",     ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Entry",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("TP",      ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("SL",      ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Order",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (auto it = g_decisions.begin(); it != g_decisions.end(); ++it) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", it->timeStr.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(
                colF(signalColor(it->decision)), "%s", it->decision.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", it->buy);
            ImGui::TableNextColumn(); ImGui::Text("%d", it->sell);
            ImGui::TableNextColumn(); ImGui::Text("%d", it->neu);
            ImGui::TableNextColumn(); ImGui::Text("%s", formatPrice(it->entry).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", formatPrice(it->tp).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", formatPrice(it->sl).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(
                it->orderPlaced ? ImVec4(0.247f, 0.725f, 0.314f, 1.0f)
                                : ImVec4(0.545f, 0.580f, 0.620f, 1.0f),
                "%s", it->orderPlaced ? "placed" : "-");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// log
// ---------------------------------------------------------------------------
static void drawLogPanel() {
    Layout L = layout();
    applyPanelPlacement(L.logPos, L.logSz);
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
    if (ImGui::BeginChild("##logscroll", ImVec2(0, 0), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        std::vector<LogEntry> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            snapshot.assign(g_log.begin(), g_log.end());
        }
        if (snapshot.empty()) {
            ImGui::TextDisabled("No log entries.");
        } else {
            for (const auto& e : snapshot) {
                ImGui::TextColored(colF(e.color), "%s", e.text.c_str());
            }
            if (g_autoScroll) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// wsl2-safe window setup
// ---------------------------------------------------------------------------
static void glfwErrorCallback(int code, const char* desc) {
    std::cerr << "GLFW error (" << code << "): " << (desc ? desc : "?") << "\n";
}

int main() {
    std::ios::sync_with_stdio(false);
    logLine("Flow++ GUI started. Close the window to stop.", col::text);

    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW.\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // normal window with titlebar (close/minimize), then maximized to fill
    // the screen so it still feels fullscreen
    GLFWwindow* window = glfwCreateWindow(1600, 1000, "Flow++ - Alpaca trading GUI",
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
    glfwMaximizeWindow(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = "flow_gui.ini";

    applyTheme();

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
        logLine(msg, isError ? col::red : col::green);
    });

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // handy way to quit quickly
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, true);

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

        if (g_forceLayout) g_forceLayout = false;

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
