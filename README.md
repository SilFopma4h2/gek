# Flow++

A C++ trading bot that processes real-time quotes from Alpaca's websocket feed, computes a buy/sell signal from order-book imbalance, and optionally places orders automatically via the Alpaca REST API.

The name *Flow++* references the flow of market data and the C++ language.

## Overview

Flow++ connects to the Alpaca IEX quote stream for a configurable symbol (default `SPY`), computes a **weighted mid price** and a volume ratio between bid and ask for every incoming quote, and derives a signal: `BUY`, `SELL` or `NEUTRAL`. Signals are collected over a **5-minute** window; the majority signal within that window decides whether a (bracket) order is placed.

## Architecture

| File | Responsibility |
|---|---|
| `main.cpp` | Entry point (console version), order-book storage, signal logic, threading |
| `gui.cpp` | Entry point for the GUI version (`flow_gui`); contains all logic from `main.cpp` in a Dear ImGui interface |
| `websocket.h` / `websocket.cpp` | Connection, authentication and subscription to Alpaca's websocket (Boost.Beast + OpenSSL) |
| `order.h` / `order.cpp` | Order placement via the Alpaca REST API (libcurl) |
| `imgui/` | Vendored Dear ImGui (GLFW + OpenGL backends) |
| `CMakeLists.txt` | Build configuration |
| `build.sh` | Build and run script |

### Dataflow

1. `client.run()` runs on a separate thread and opens a websocket connection to `stream.data.alpaca.markets`.
2. For every incoming quote (`type == "q"`) the callback is invoked, which stores the bid/ask price and volume in `Apple_Book_Data` (a `std::deque`, limited to 500 entries).
3. The main thread is woken via a `condition_variable` as soon as new data is available, and calls `evaluateSignal()` on the latest quote.
4. Every **5 minutes** (set by `DECISION_INTERVAL_SECONDS` in `main.cpp`) the majority of the collected signals is determined with `majoritySignal()`. On a `BUY`/`SELL` majority a bracket order is placed via `order()`; afterwards the signal buffer and spread sum are reset.

### Signal logic

For every quote the following is computed:

- **Mid price**: `(bid + ask) / 2`
- **Weighted mid price (wmid)**: price weighted towards the volume on the opposite side
- **Ratio**: share of the bid volume in the total volume

Rules:

- `wmid > mid` and `ratio > 0.60` → **BUY** (buyer pressure dominant)
- `wmid < mid` and `ratio < 0.40` → **SELL** (seller pressure dominant)
- otherwise → **NEUTRAL**

### Decision (every 5 minutes)

A signal is added to `signalBuffer` for each incoming quote. Once the **5-minute** window (`DECISION_INTERVAL_SECONDS`) has elapsed:

1. `majoritySignal()` counts which signal occurred most often → the decision.
2. On `BUY` or `SELL`, `order(decision, tickCount)` places a limit-bracket order at the current mid price, with TP/SL based on the **average spread** over the window (a volatility measure).
3. Buffers and the spread sum are reset for the next window.

Because this is a time window, the number of ticks per window varies; the average spread is therefore divided by the actual number of processed ticks.

## Dependencies

- CMake ≥ 3.20
- C++20 compiler
- Boost (system)
- OpenSSL
- nlohmann/json
- libcurl
- Threads

For the GUI (`flow_gui`) additionally:

- GLFW 3 (via pkg-config)
- OpenGL / Mesa

## Build & Run

```bash
./build.sh            # builds and starts the console version (flow)
./build.sh --gui      # builds and starts the GUI version (flow_gui)
```

`build.sh`:

1. Reads the Alpaca API credentials from the environment
2. Configures and builds the project with CMake
3. Starts the `flow` executable, or `flow_gui` with `--gui`

Manual build:

```bash
cmake -S . -B build
cmake --build build
./build/flow            # console version
./build/flow_gui        # GUI version
```

## GUI (`flow_gui`)

A Dear ImGui + GLFW + OpenGL interface with all functionality from `main.cpp`:

- **Connection & Settings**: configurable symbol, Connect/Disconnect, API-key status, order toggle, TP/SL multipliers and the decision interval (minutes).
- **Market Data**: live bid/ask/spread/mid/weighted mid/bid-ratio, volume bar and a mid-price sparkline.
- **Signals**: current signal, per-signal counts in the running window, countdown to the next decision and a "Decide now" button.
- **Decisions**: table of recent decisions (time, majority, entry/TP/SL, order status).
- **Log**: scrolling, colored log (feed status, quotes, decisions, order results) with autoscroll and a "log every quote" toggle.

Like `main.cpp`, the decision is made as soon as the interval (default 5 minutes) has elapsed; the majority signal then determines whether a bracket order is placed. Orders run on a separate thread so the UI stays responsive.

## WSL2

`flow_gui` is **WSL2-proof**: it runs on WSLg (X11/Wayland) via GLFW + OpenGL (Mesa). If no display is available (no WSLg / no X server) it prints a clear error message and exits cleanly. Requirements:

```bash
sudo apt install libglfw3-dev libgl1-mesa-dev libcurl4-openssl-dev libssl-dev \
     libboost-system-dev nlohmann-json3-dev
```

Run the GUI from an interactive WSL shell (so `DISPLAY` is set), start WSLg or an X server, and run `./build/flow_gui`.

## Configuration

- In the console version (`main.cpp`) the symbol is hardcoded (`const std::string symbol = "SPY";`); in the GUI it can be changed via the panel.
- The API endpoint in `order.cpp` points to Alpaca's **paper trading** environment (`paper-api.alpaca.markets`), so no real money is involved.

## Credentials

Set the following environment variables before running:

```bash
export ALPACA_API_KEY=your_api_key
export ALPACA_API_SECRET=your_api_secret
```

Flow++ reads these at runtime; they are never hardcoded. Keep them out of version control.

## Known considerations

- **No position check**: there is no check for existing open positions or orders. If the BUY/SELL signal keeps dominating, multiple bracket orders can build up. Consider adding a position check before going live with real money.
- **Order frequency**: `order()` is called at most **once per 5 minutes** (the time window). If the majority signal is `NEUTRAL`, no order is placed.
- **GUI state**: window layout is persisted in `flow_gui.ini` (generated at runtime, gitignored).
