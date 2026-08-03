# gek

De naam gek komt van dat het makkelijker te zoeken is met cd gek in plaats van een lange naam.

Een C++ trading bot die real-time quotes van Alpaca's websocket-feed verwerkt, een koop/verkoop-signaal berekent op basis van orderboek-onbalans, en (optioneel) automatisch orders plaatst via de Alpaca REST API.

## Overzicht

`gek` verbindt met de Alpaca IEX quote-stream voor een opgegeven symbool (standaard `SPY`), berekent per binnenkomende quote een **weighted mid price** en volume-ratio tussen bid en ask, en bepaalt op basis daarvan een signaal: `BUY`, `SELL` of `NEUTRAAL`.

## Architectuur

| Bestand | Verantwoordelijkheid |
|---|---|
| `main.cpp` | Entry point, orderbook-opslag, signaallogica, threading |
| `websocket.h` / `websocket.cpp` | Verbinding, authenticatie en subscriptie op Alpaca's websocket (Boost.Beast + OpenSSL) |
| `order.h` / `order.cpp` | Order plaatsen via Alpaca REST API (libcurl) |
| `CMakeLists.txt` | Build-configuratie |
| `build.sh` | Build- en run-script |

### Dataflow

1. `startAlpacaFeed()` draait in een aparte thread en opent een websocket-verbinding met `stream.data.alpaca.markets`.
2. Bij elke binnenkomende quote (`type == "q"`) wordt de callback aangeroepen, die bid/ask prijs en volume opslaat in `Apple_Book_Data` (een `std::deque`, gelimiteerd tot 500 entries).
3. De hoofdthread wordt via een `condition_variable` gewekt zodra nieuwe data beschikbaar is, en roept `evaluateSignal()` aan op de laatste quote.

### Signaallogica

Voor elke quote wordt berekend:

- **Mid price**: `(bid + ask) / 2`
- **Weighted mid price (wmid)**: prijs gewogen naar volume aan de tegenoverliggende kant
- **Ratio**: aandeel van het bid-volume in het totale volume

Regels:
- `wmid > mid` én `ratio > 0.60` → **BUY** (kopersdruk dominant)
- `wmid < mid` én `ratio < 0.40` → **SELL** (verkopersdruk dominant)
- anders → **NEUTRAAL**

## Dependencies

- CMake ≥ 3.20
- C++20 compiler
- Boost (system)
- OpenSSL
- nlohmann/json
- libcurl
- Threads

## Build & Run

```bash
./build.sh
```

Dit script:
1. Zet de Alpaca API-credentials als environment variables
2. Configureert en bouwt het project met CMake
3. Start de resulterende `gek`-executable

Handmatig:

```bash
cmake -S . -B build
cmake --build build
./build/gek
```

## Configuratie

- Het te volgen symbool staat hardcoded in `main.cpp` (`const std::string symbol = "SPY";`).
- De API endpoint in `order.cpp` wijst naar de **paper trading** omgeving van Alpaca (`paper-api.alpaca.markets`), dus orders worden niet met echt geld uitgevoerd.

## Bekende aandachtspunten

- **API keys**: staan niet hardcoded in `build.sh` — die worden via `ALPACA_API_KEY` / `ALPACA_API_SECRET` environment variables ingelezen in `main.cpp` en `order.cpp`. Zorg dat deze buiten versiebeheer blijven (bv. via je shell-profiel of een niet-gecommit `.env`).
- **`order()`-functie**: plaatst correct `"buy"` bij een BUY-signaal en `"sell"` bij een SELL-signaal.
- **`order()` wordt nog nergens aangeroepen**: `evaluateSignal()` retourneert wel een signaal, maar in de `main()`-loop wordt dat signaal alleen berekend en geprint — het wordt niet doorgegeven aan `order()`. Er worden dus nog geen daadwerkelijke orders geplaatst. Om dit te activeren zou je in `main()` iets als `std::string signal = evaluateSignal(latestBook); order(signal);` moeten toevoegen.
- **Geen rate limiting / order-throttling**: zodra bovenstaande is toegevoegd, zou bij elke quote met een geldig signaal een order geplaatst worden. Bij een actieve feed kan dat al snel tot zeer veel orders leiden — overweeg een cooldown of positie-check voordat je live gaat.

## Taal

Comments en logmeldingen zijn in het Nederlands; code en identifiers in het Engels.