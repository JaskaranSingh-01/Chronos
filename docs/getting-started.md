# Getting started

Two ways in: Docker, for the Trading Terminal, the Strategy Lab and the Strategies page in one command; or a native
build, for the CLI, the tests and the benchmarks.

## Docker

Needs Docker with Compose. From the repository root:

```bash
docker compose up
```

The first run builds the image (a few minutes: the UI, then `chronos-serve`, `simulator` and `chronos-e2e-fixture` with
g++ 14, Release). Open `http://localhost:8080/` and enter the token `chronos`. After pulling new code,
`docker compose up --build` rebuilds the image; a plain `up` reuses the one it has.

- **No data needed to start.** With no ITCH day in `data/itch`, the container generates two small sample days (ACME and
  ZETA, the end-to-end tests' fixture) and serves those: load `ACME`, play it, place an order, run the Lab's market
  maker on it (+$3.00 over 6 fills).
- **The real day.** Download it into `data/itch` ([below](#get-the-data)) and run `docker compose up` again. The
  directory is mounted read-only; the first start builds each day into the per-symbol store
  ([market-data.md](market-data.md#the-per-symbol-store)), a third of its size and a few minutes a day in Docker on a
  Mac (about one natively), before the server listens. The store is kept in the `chronos-state` volume with the Lab's
  runs, the scripts saved on the Strategies page and the session logs.
- **The CLI in the image:**

  ```bash
  docker compose run --rm --entrypoint simulator chronos replay /data/12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30
  ```

- `CHRONOS_TOKEN` sets another token. Arguments after the service go to `chronos-serve`, e.g.
  `docker compose run --rm --service-ports chronos --allow-truncated-tail` to serve a day cut mid-message (it must still
  be named `MMDDYYYY.NASDAQ_ITCH50.bin`).

The compose file publishes the port on `127.0.0.1` only. Inside the container `chronos-serve` binds every interface,
which is why it wants a token ([server.md](server.md#security)).

## Native build

**Requirements:** CMake 3.20 or newer, a C++20 compiler and a POSIX system; Node 24 and npm for the UI. Developed on
macOS arm64 with Apple clang 17; CI builds with g++ and clang on Ubuntu and Apple clang on macOS. GoogleTest, Google
Benchmark (with `-DCHRONOS_BENCH=ON`), Crow, asio, nlohmann/json, zstd, xxHash and Lua 5.4 come in through FetchContent,
so the first configure needs network access. At run time the engine needs only the C++ standard library and POSIX.

```bash
cmake -S . -B build                  # RelWithDebInfo unless CMAKE_BUILD_TYPE is set
cmake --build build -j
ctest --test-dir build               # 723 tests
(cd web && npm ci && npm run build)  # the UI, into web/dist
```

## Get the data

NASDAQ publishes sample TotalView-ITCH 5.0 days at `https://emi.nasdaq.com/ITCH/Nasdaq ITCH/`. These docs use
2019-12-30:

```bash
mkdir -p data/itch
curl -fL -o data/itch/12302019.NASDAQ_ITCH50.gz 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz'
gzip -t data/itch/12302019.NASDAQ_ITCH50.gz
gunzip -c data/itch/12302019.NASDAQ_ITCH50.gz > data/itch/12302019.NASDAQ_ITCH50.bin
```

3.5 GB to download, 8.25 GB unpacked. An optional 256 MiB slice for quick runs is in
[market-data.md](market-data.md#getting-the-data).

## First runs

```bash
# rebuild every symbol's book over the whole day (about 54 s on an Apple M4)
./build/simulator replay data/itch/12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30

# backtest the market maker on SPY
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy market_maker

# the Trading Terminal, the Strategy Lab and the Strategies page
./build/chronos-serve --data data/itch          # open http://127.0.0.1:8080/, type AAPL, press Enter

# once chronos-serve has built the store: one symbol's day, read in under a second
./build/simulator backtest --store data/itch/store --symbol AAPL --date 2019-12-30 --strategy orb
```

Next: the README's [Using it](../README.md#using-it) for a numbered tour of each page,
[architecture/](architecture/README.md) for how the parts fit, [cli.md](cli.md) for every option.
