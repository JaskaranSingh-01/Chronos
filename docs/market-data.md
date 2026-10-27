# Market data: events, sources and the per-symbol store

## One event type for every source

Every source emits `MarketEvent`, 48 bytes:

| Field | Type |
|---|---|
| timestamp | `int64` ns since the Unix epoch, UTC |
| order id, new order id | `uint64` |
| price | `int64` ticks at the instrument's price scale |
| quantity | `uint64` shares |
| symbol | `SymbolId` (`uint32`), registered in `InstrumentRegistry` with its price and quantity scales |
| type, side, trading state | `uint8` each; the trading state only on Status events |

Nine types: Add, Modify, Replace, Cancel, Delete, Execute, Clear, Trade and Status (a book's trading state). Clear
empties a book; no ITCH message maps to it. Modify exists for future sources, and `MarketBook` rejects it as
`Unsupported`.

Sources implement `MarketEventSource::next(MarketEvent&)`: `ItchSource` (the CLI's and the server's), `CsvSource` (the
library's and the tests'), and `VectorSource` (the tests'). The engine never sees a wire format.

**The conformance suite.** Every source must pass `SourceConformanceTest`: timestamps never go backwards, references
name live orders of an earlier Add, sizes stay within the remaining quantity, symbols are registered first, the same
bytes give the same events, and errors are sticky, with a position. It is instantiated for all three sources.

## NASDAQ TotalView-ITCH 5.0

`ItchSource` maps the day file (`MappedFile`, `mmap`) and frames it with `ItchMessageReader`:

- It reads each message's stock-locate field before decoding, so messages of unselected symbols are never decoded.
- The `R` (stock directory) message of each selected symbol registers its instrument: venue XNAS, price scale 4.
- `A`/`F` become Add, `U` Replace, `X` Cancel, `D` Delete, `E`/`C` Execute (`C` carries its execution price), `P`
  Trade (which the book ignores), and `H` Status.
- ITCH times are ns after midnight; the source adds the session's midnight, from `--date` and the US Eastern offset of
  that date, or `--utc-offset`.
- A file cut mid-message is an error unless `--allow-truncated-tail` is given.

`ItchBuilderTest.*MatchesRealNasdaqBytes` proves the test-side ITCH builder reproduces real NASDAQ messages byte for
byte, so every ITCH test runs on the real wire layout.

**The trade an event prints.** `market_trade()` ([`market/trade.hpp`](../include/simulator/market/trade.hpp)) reads it
before the book applies the event: an `E` at the resting order's price (the book still holds the order), a `C` at its
own, a `P` as given. The server's 1 s candles and the ORB strategy both use it.

## Getting the data

NASDAQ publishes sample TotalView-ITCH 5.0 days at `https://emi.nasdaq.com/ITCH/Nasdaq ITCH/`, one gzip file per day
named `MMDDYYYY.NASDAQ_ITCH50.gz`. These docs use 2019-12-30:

```bash
mkdir -p data/itch
curl -fL -o data/itch/12302019.NASDAQ_ITCH50.gz 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/12302019.NASDAQ_ITCH50.gz'
gzip -t data/itch/12302019.NASDAQ_ITCH50.gz
gunzip -c data/itch/12302019.NASDAQ_ITCH50.gz > data/itch/12302019.NASDAQ_ITCH50.bin
```

The download is 3,524,013,057 bytes (on 2026-10-13), and the `.bin` 8,251,407,909 bytes with 268,744,780 messages.
Chronos maps the uncompressed file; it does not read gzip. `data/` is in `.gitignore`: market data is never committed
or redistributed here.

An optional slice, the first 256 MiB, for quick runs and the real-data tests:

```bash
head -c 268435456 data/itch/12302019.NASDAQ_ITCH50.bin > data/itch/12302019.NASDAQ_ITCH50.first-256MiB.bin
```

It covers 04:00 to 09:30:10 ET, so it is pre-market only, and its last message is cut, so pass
`--allow-truncated-tail`.

## The per-symbol store

Replaying one symbol from the day file means reading the whole 8.25 GB file to find its messages. The store fixes
that: `chronos-serve` reads each new day once, about a minute, and writes every symbol's share of it to a file of its
own.

- **Layout.** One zstd file per symbol, `<SYMBOL>.itch.zst`, holding each day's segment as zstd frames: every ITCH
  frame of the symbol's stock locate from its directory entry on, and every frame that reaches every symbol (each
  System Event, each frame no locate routes, a cut tail), in file order. An `index.tsv` lists each segment's day,
  place, size and XXH3-128 hash.
- **Size.** About a third of the days it holds: 2.8 GiB for 2019-12-30. A build needs free space of at least 60 % of
  its day file; a day without it, or whose build fails, is skipped with a reason on stderr.
- **Reads.** Every replay of a symbol, the terminal's, a Lab run's, Verify's and Watch's, and `simulator --store`,
  reads only that symbol's segments: AAPL's day is about 1 % of the file. A segment is checked as it is read (its
  range, each zstd frame's checksum and its hash), and a damaged one fails naming the symbol and day.
- **Versions.** Each day file's XXH3-128 hash, and the regular hours its own System Events set, are kept in
  `runs/fingerprints.json`. A day file that changes is built again; the Lab's runs record each segment's hash, so a
  change to one symbol's segment does not disturb another's runs.
- **Builds are crash-safe.** A build that does not commit cuts every file it wrote back to its committed end; the index
  is replaced through a rename. One `chronos-serve` at a time holds the store's lock (`.lock`).
- **Memory.** A build's buffers are capped: a symbol's buffer is written once it holds enough, and the largest are
  written first before the total would pass the cap.

```bash
./build/simulator replay --store data/itch/store --symbol AAPL --date 2019-12-30
./build/simulator backtest --store data/itch/store --symbol AAPL --date 2019-12-30 --strategy orb
```

To rebuild every day, delete `<store>/index.tsv` and restart `chronos-serve`. A rebuilt day's old segments stay in
their files until then (about a third of the day each rebuild).
