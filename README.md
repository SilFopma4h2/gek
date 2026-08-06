# Flow++

A C++ trading bot. It reads live quotes from Alpaca's websocket feed, works out a buy/sell signal from order-book imbalance, and can place orders automatically via the Alpaca REST API.

The name is a nod to both the flow of market data and C++. The project is called **Flow++**, but GitHub doesn't allow a `+` in repo names, so the repo itself is called `flowpp`.

## What it does

Connects to the Alpaca IEX quote stream for a symbol (default `SPY`). For every quote it computes a **weighted mid price** and the bid/ask volume ratio, then classifies it as `BUY`, `SELL` or `NEUTRAL`. Signals pile up over a **5-minute** window; whichever signal shows up most often decides whether a (bracket) order goes out.

## License

MIT-style-ish, but shorter. You may use, modify and share the code as long as you keep this copyright notice and your modified version stays open source (no closed-source forks without my permission). See [LICENSE](LICENSE).

## Layout

| File | What it does |
|---|---|
| `main.cpp` | Entry point (console version), order-book storage, signal logic, threading |
| `gui.cpp` | Entry point for the GUI version (`flow_gui`); same logic as `main.cpp`, in a Dear ImGui UI |
| `websocket.h` / `websocket.cpp` | Connects, authenticates and subscribes to Alpaca's websocket (Boost.Beast + OpenSSL) |
| `order.h` / `order.cpp` | Place orders via the Alpaca REST API (libcurl) |
| `imgui/` | Vendored Dear ImGui (GLFW + OpenGL backends) |
| `CMakeLists.txt` | Build config |
| `build.sh` | Build and run |

### Dataflow

1. `client.run()` runs on its own thread and opens a websocket to `stream.data.alpaca.markets`.
2. Every quote (`type == "q"`) hits the callback, which stores the bid/ask price and volume in `symbolOrderBook` (a `std::deque`, capped at 500).
3. The main thread wakes on a `condition_variable` whenever new data shows up, then calls `evaluateSignal()` on the latest quote.
4. Every **5 minutes** (`DECISION_INTERVAL_SECONDS` in `main.cpp`) `majoritySignal()` picks the most common signal. A `BUY`/`SELL` majority sends a bracket order via `order()`; then the signal buffer and spread sum get reset.

### Signal logic — the formulas

Every incoming quote carries four numbers: `bid`, `ask`, `bid_volume`, `ask_volume`.
From those the base signal computes:

```
spread = ask - bid
mid    = (bid + ask) / 2
total  = bid_volume + ask_volume

wmid   = (bid_volume * ask + ask_volume * bid) / total      (if total > 0)
ratio  = bid_volume / total                                  (if total > 0)
```

- **`mid`** is the plain midpoint of the spread.
- **`wmid`** is the *microprice* / weighted mid. It's a real volume-weighted
  average of the two prices, so it sits *inside* the spread. A heavy bid side
  (`bid_volume > ask_volume`) pulls `wmid` up towards the ask, and vice versa.
- **`ratio`** is the bid's share of the total top-of-book size (0.5 = perfectly
  balanced).

Per-quote signal:

```
wmid > mid  AND  ratio > 0.60   -> BUY    (bid side dominates)
wmid < mid  AND  ratio < 0.40   -> SELL   (ask side dominates)
otherwise                       -> NEUTRAL
```

> Note: because `wmid > mid` is mathematically equivalent to `ratio > 0.5`, the
> `ratio` threshold is what actually decides — `0.60`/`0.40` is just a stricter
> margin on top of the imbalance direction.

**Zero-size fallback (momentum).** The free Alpaca **IEX** feed frequently sends
`bid_volume`/`ask_volume` as `0`. With no size info `wmid == mid` and `ratio == 0`,
so the volume rules would *always* return NEUTRAL. To avoid that the bot falls
back to **mid-price momentum** per quote:

```
momentum = mid_now - mid_prev
tick     = max(0.01, spread * 0.5)

momentum >  tick  -> BUY
momentum < -tick  -> SELL
otherwise          -> NEUTRAL
```

So even without size data the bot still reacts to price moving up or down by at
least half a tick (with a 1-cent floor).

### Decision (every 5 minutes)

Once the **5-minute** window (`DECISION_INTERVAL_SECONDS`) is up, four **experts**
vote and the results combine:

- **Base expert** — majority of the per-quote signals in the window
  (`majorityFromCounts`): whichever of BUY / SELL / NEUTRAL appears most often,
  with BUY winning a 3-way draw.
- **OFI (order-flow imbalance)** — cumulative signed size flow, normalised by
  total volume:
  ```
  ofi += (bid >= prev.bid ?  Δbid_size : 0) - (ask <= prev.ask ? Δask_size : 0)
  n    =  ofi / totalVolume
  n >  +OFI_THRESHOLD  -> BUY        n < -OFI_THRESHOLD  -> SELL
  ```
- **Micro-price drift** — cumulative `(wmid - mid)`, expressed as a fraction of
  the average spread so it's scale-free:
  ```
  n = Σ(wmid - mid) / (tickCount * avgSpread)      avgSpread = Σspread / tickCount
  ```
- **Liquidity absorption** — passively added minus cancelled size per side,
  normalised by volume, a spoofing/pull detector.

All three expert thresholds (`OFI`, drift, absorption) default to
`0.05 / 0.10 / 0.05` and are adjustable live in the GUI.

**Combining the votes** (`combinedDecision`):

```
if  buy_votes >= 2  AND  buy > sell   -> BUY
if  sell_votes >= 2 AND  sell > buy   -> SELL
otherwise                              -> follow the base expert
```

A clear 2+ expert consensus carries the decision; otherwise the base expert's
vote decides, so a lone BUY/SELL never gets silently dropped just because the
other experts sat out on NEUTRAL.

On a non-NEUTRAL decision, a limit-bracket order is placed at the current mid:

```
avgSpread = spreadSum / tickCount        (average spread this window)
TP = entry + 3.0  * avgSpread            (take-profit, 3x the avg spread)
SL = entry - 1.5  * avgSpread            (stop-loss,   1.5x the avg spread)
```

(2:1 reward:risk; reversed for SELL.) Orders are only sent if the **orders
toggle is ON** in the GUI. Buffers, expert tallies and the spread sum all reset
for the next window. Ticks per window vary — it's a time window, not a count —
so averages divide by however many ticks actually came in.

## Requirements

All libs are found via CMake's `find_package`. On Debian/Ubuntu (incl. WSL) they come from apt:

| Library | What it's used for | apt package |
|---|---|---|
| CMake ≥ 3.20 | build system | `cmake` |
| C++20 compiler | the code itself | `g++` |
| Boost.System | websocket (Boost.Asio / Beast) | `libboost-system-dev` |
| OpenSSL | TLS for the websocket and REST calls | `libssl-dev` |
| nlohmann/json | JSON for websocket frames and order bodies | `nlohmann-json3-dev` |
| libcurl | placing orders via the Alpaca REST API | `libcurl4-openssl-dev` |
| Threads | std::thread / mutexes / cv | (part of libc, nothing to install) |

For the GUI (`flow_gui`) you also need:

| Library | What it's used for | apt package |
|---|---|---|
| GLFW 3 | window + input (found via pkg-config) | `libglfw3-dev` |
| OpenGL / Mesa | rendering (Dear ImGui draw list) | `libgl1-mesa-dev` |

One-shot install for everything:

```bash
sudo apt install cmake g++ libboost-system-dev libssl-dev \
     nlohmann-json3-dev libcurl4-openssl-dev libglfw3-dev libgl1-mesa-dev
```

## Build & Run

```bash
./build.sh            # builds and starts the console version (flow)
./build.sh --gui      # builds and starts the GUI version (flow_gui)
```

`build.sh`:

1. Reads the Alpaca API credentials from the environment
2. Configures and builds with CMake
3. Runs `flow`, or `flow_gui` when you passed `--gui`

Manual build:

```bash
cmake -S . -B build
cmake --build build
./build/flow            # console version
./build/flow_gui        # GUI version
```

## GUI (`flow_gui`)

A Dear ImGui + GLFW + OpenGL UI covering everything `main.cpp` does:

- **Connection & Settings**: symbol, Connect/Disconnect, API-key status, order toggle, TP/SL multipliers, decision interval (minutes).
- **Market Data**: live bid/ask/spread/mid/weighted mid/bid-ratio, a volume bar and a mid-price sparkline.
- **Signals**: current signal, per-signal counts in the current window, countdown to the next decision and a "Decide now" button.
- **Decisions**: a table of recent decisions (time, majority, entry/TP/SL, order status).
- **Log**: scrolling, colored log (feed status, quotes, decisions, order results) with autoscroll and a "log every quote" toggle.

Same timing as `main.cpp`: the decision fires as soon as the interval (default 5 minutes) elapses, and the majority signal decides whether an order is placed. Orders run on a worker thread so the UI doesn't stall.

## WSL2

`flow_gui` is **WSL2-proof**: it runs on WSLg (X11/Wayland) via GLFW + OpenGL (Mesa). Without a display (no WSLg / no X server) it prints an error and exits cleanly. Install the deps from [Requirements](#requirements) first.

Run the GUI from an interactive WSL shell (so `DISPLAY` is set), with WSLg or an X server running, then `./build/flow_gui`.

## Configuration

- Console version (`main.cpp`): the symbol is hardcoded (`const std::string symbol = "SPY";`); the GUI lets you change it in the panel.
- The endpoint in `order.cpp` is Alpaca's **paper trading** environment (`paper-api.alpaca.markets`), so no real money moves.

## Credentials

Set these before running:

```bash
export ALPACA_API_KEY=your_api_key
export ALPACA_API_SECRET=your_api_secret
```

Flow++ reads them at runtime, never hardcodes them. Don't commit them.

## GOTCHAs / caveats

- **No position check**: nothing looks at open positions or orders first. If BUY or SELL keeps dominating, bracket orders can pile up across windows. Worth adding a position check before feeding it real money.
- **Order rate**: `order()` runs at most **once per 5 minutes** (one decision per window). `NEUTRAL` → no order.
- **GUI state**: window layout persists in `flow_gui.ini` (created at runtime, gitignored).
