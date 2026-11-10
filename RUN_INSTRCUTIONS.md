# Run instructions

Chronos replays a real NASDAQ trading day (every order, in order) and lets you trade or backtest strategies against it
in the browser.

## 1. Run it (Docker, 3 commands)

Needs [Docker Desktop](https://www.docker.com/products/docker-desktop/).

```bash
git clone https://github.com/JaskaranSingh-01/Chronos.git && cd Chronos
docker compose up                # first run builds the image: a few minutes
# then open http://localhost:8080 and enter the token: chronos
```

With no market data it serves two small generated sample days (symbols `ACME` and `ZETA`), so the UI works at once.
Stop with Ctrl+C.

## 2. Real market data (optional)

NASDAQ's free sample day, 2019-12-30: about 3.5 GB to download, 8.25 GB unpacked; allow about 11 GB of disk.

```bash
mkdir -p data/itch
curl -fL 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz' | gunzip > data/itch/12302019.NASDAQ_ITCH50.bin
docker compose up                # reads the day into its store once (a few minutes), then serves it
```

Place day files in `data/itch/`, named `MMDDYYYY.NASDAQ_ITCH50.bin`. Any day from that NASDAQ folder works.

## 3. Using it

Three pages, switched at the top left:

- **Trading Terminal** (`http://localhost:8080/`): type a symbol (`ACME`, or `AAPL` with real data) and press Enter.
  Use **Play**, **Step** and the speed buttons. Click a price in the order book, then **Place** an order. It joins
  the real queue, and you see the shares ahead of you until it fills.
- **Strategy Lab** (`#lab`): pick a strategy, symbol and days, then **Run backtest**. Results show PnL, fills and
  drawdown. **Verify** re-runs a backtest and checks it gives exactly the same result.
- **Strategies** (`#strategies`): write a strategy in Lua in the browser; it then appears in the Lab.

## 4. From source (optional, for the C++ engine and tests)

Needs CMake 3.20+, a C++20 compiler (Xcode tools on macOS, `build-essential` on Ubuntu) and Node 24.

```bash
cmake -S . -B build && cmake --build build -j                  # engine, server, tests
ctest --test-dir build                                         # run the test suite
(cd web && npm ci && npm run build)                            # the UI
./build/chronos-serve --data data/itch                         # then open http://127.0.0.1:8080
./build/simulator --help                                       # the command-line replay and backtest tool
```

## Where to look

`README.md`: overview and benchmark results · `docs/`: design and guides · `include/`, `src/`: C++ engine ·
`server/`: web server · `web/`: browser UI · `strategies/`: trading strategies · `tests/`: tests.
