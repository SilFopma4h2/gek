# gek

De naam gek komt van dat het makkelijker te zoeken is met cd gek in plaats van een lange naam.

Een C++ trading bot die real-time quotes van Alpaca's websocket-feed verwerkt, een koop/verkoop-signaal berekent op basis van orderboek-onbalans, en (optioneel) automatisch orders plaatst via de Alpaca REST API.

## Overzicht

`gek` verbindt met de Alpaca IEX quote-stream voor een opgegeven symbool (standaard `SPY`), berekent per binnenkomende quote een **weighted mid price** en volume-ratio tussen bid en ask, en bepaalt op basis daarvan een signaal: `BUY`, `SELL` of `NEUTRAAL`. De signalen worden over een venster van **5 minuten** verzameld; het meerderheidssignaal binnen dat venster beslist of er een (bracket) order wordt geplaatst.

## Architectuur

| Bestand | Verantwoordelijkheid |
|---|---|
| `main.cpp` | Entry point (console-versie), orderbook-opslag, signaallogica, threading |
| `gui.cpp` | Entry point GUI-versie (`gek_gui`); bevat alle logica uit `main.cpp` in een Dear ImGui interface |
| `websocket.h` / `websocket.cpp` | Verbinding, authenticatie en subscriptie op Alpaca's websocket (Boost.Beast + OpenSSL) |
| `order.h` / `order.cpp` | Order plaatsen via Alpaca REST API (libcurl) |
| `imgui/` | Vendored Dear ImGui (GLFW + OpenGL backends) |
| `CMakeLists.txt` | Build-configuratie |
| `build.sh` | Build- en run-script (console-versie) |

### Dataflow

1. `client.run()` draait in een aparte thread en opent een websocket-verbinding met `stream.data.alpaca.markets`.
2. Bij elke binnenkomende quote (`type == "q"`) wordt de callback aangeroepen, die bid/ask prijs en volume opslaat in `Apple_Book_Data` (een `std::deque`, gelimiteerd tot 500 entries).
3. De hoofdthread wordt via een `condition_variable` gewekt zodra nieuwe data beschikbaar is, en roept `evaluateSignal()` aan op de laatste quote.
4. Iedere **5 minuten** (aangegeven door `DECISION_INTERVAL_SECONDS` in `main.cpp`) wordt de meerderheid van de verzamelde signalen bepaald met `majoritySignal()`. Bij een `BUY`/`SELL`-meerderheid wordt via `order()` een bracket order geplaatst; daarna worden signaalbuffer en spread-som gereset.

### Signaallogica

Voor elke quote wordt berekend:

- **Mid price**: `(bid + ask) / 2`
- **Weighted mid price (wmid)**: prijs gewogen naar volume aan de tegenoverliggende kant
- **Ratio**: aandeel van het bid-volume in het totale volume

Regels:
- `wmid > mid` én `ratio > 0.60` → **BUY** (kopersdruk dominant)
- `wmid < mid` én `ratio < 0.40` → **SELL** (verkopersdruk dominant)
- anders → **NEUTRAAL**

### Beslissing (elke 5 minuten)

Per binnenkomende quote wordt een signaal aan `signalBuffer` toegevoegd. Zodra het tijdvenster van **5 minuten** (`DECISION_INTERVAL_SECONDS`) verstreken is:

1. `majoritySignal()` telt welk signaal het vaakst voorkwam → de beslissing.
2. Bij `BUY` of `SELL` plaatst `order(decision, tickCount)` een limit-bracket order op de actuele mid-price, met TP/SL gebaseerd op de **gemiddelde spread** over het venster (volatiliteitsmaat).
3. Buffers en spread-som worden gereset voor het volgende venster.

Omdat het om een tijdvenster gaat, varieert het aantal ticks per venster; de gemiddelde spread wordt daarom door het werkelijk aantal verwerkte ticks gedeeld.

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
./build/gek            # console-versie
./build/gek_gui        # GUI-versie
```

## GUI (`gek_gui`)

Een Dear ImGui + GLFW + OpenGL interface met alle functionaliteit uit `main.cpp`:

- **Verbinding & Instellingen**: symbool (aanpasbaar), Connect/Disconnect, API-key status, `orderyes`-toggle, TP/SL multipliers en het beslissingsinterval (minuten).
- **Markt Data**: live bid/ask/spread/mid/weighted mid/bid-ratio, volume-balk en een mid-price-sparkline.
- **Signalen**: huidig signaal, tellingen per signaal in het lopende venster, aftelklok tot de volgende beslissing en een "Beslis nu"-knop.
- **Beslissingen**: tabel met recente beslissingen (tijd, meerderheid, entry/TP/SL, order-status).
- **Log**: scrollende, gekleurde log (feed-status, quotes, beslissingen, orderresultaten) met autoscroll en een "log elke quote"-toggle.

De beslissing wordt, net als in `main.cpp`, genomen zodra het interval (standaard 5 minuten) is verstreken; het meerderheidssignaal bepaalt dan of er een bracket order wordt geplaatst. Orders lopen op een aparte thread zodat de UI responsief blijft.

## WSL2

`gek_gui` is **WSL2-proof**: hij draait op WSLg (X11/Wayland) via GLFW + OpenGL (Mesa). Als er geen display beschikbaar is (geen WSLg / geen X-server), geeft hij een duidelijke foutmelding en sluit netjes af. Vereisten:

```bash
sudo apt install libglfw3-dev libgl1-mesa-dev libcurl4-openssl-dev libssl-dev \
     libboost-system-dev nlohmann-json3-dev
```

Run de GUI vanuit een interactieve WSL-shell (zodat `DISPLAY` is gezet), start WSLg of een X-server, en voer `./build/gek_gui` uit.

## Configuratie

- In de console-versie (`main.cpp`) staat het symbool hardcoded (`const std::string symbol = "SPY";`); in de GUI is dit via het paneel aanpasbaar.
- De API endpoint in `order.cpp` wijst naar de **paper trading** omgeving van Alpaca (`paper-api.alpaca.markets`), dus orders worden niet met echt geld uitgevoerd.

## Bekende aandachtspunten

- **API keys**: staan niet hardcoded in `build.sh` — die worden via `ALPACA_API_KEY` / `ALPACA_API_SECRET` environment variables ingelezen in `main.cpp` en `order.cpp`. Zorg dat deze buiten versiebeheer blijven (bv. via je shell-profiel of een niet-gecommit `.env`).
- **`order()`-functie**: plaatst correct `"buy"` bij een BUY-signaal en `"sell"` bij een SELL-signaal.
- **Orderfrequentie**: `order()` wordt maximaal **1× per 5 minuten** aangeroepen (het tijdvenster). Als het meerderheidssignaal `NEUTRAAL` is, wordt er geen order geplaatst.
- **Geen positie-check**: er wordt niet gecontroleerd of er al open posities of orders openstaan. Als het BUY/SELL-signaal blijft domineren, kunnen er meerdere bracket orders naast elkaar ontstaan — overweeg een positie-check voordat je met echt geld live gaat.

## Taal

Comments en logmeldingen zijn in het Nederlands; code en identifiers in het Engels.