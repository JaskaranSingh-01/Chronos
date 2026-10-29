# Code map

Where the design in [`architecture/`](architecture/README.md) lives in the code: each component's types, where each
invariant is enforced and tested, the repository layout, and the files to read first.

## Components in the code

The components are those of [system-architecture.md](architecture/system-architecture.md).

| Component | Code |
|---|---|
| ITCH reader | `ItchSource` ([`itch/`](../include/simulator/itch/)): framing by `ItchMessageReader` and `ByteReader`, decoding by `ItchDecoder`; files mapped by `MappedFile` ([`io/`](../include/simulator/io/)) |
| CSV reader, test source | `CsvSource` ([`csv/`](../include/simulator/csv/)), `VectorSource` ([`market/`](../include/simulator/market/)) |
| The event contract | `MarketEvent`, `MarketEventSource` and `SourceError` ([`market/`](../include/simulator/market/)); the conformance suite in [`tests/support/`](../tests/support/) |
| Instrument registry | `InstrumentRegistry` ([`market/`](../include/simulator/market/)) |
| Per-symbol store | the index, the segment reader and `StoreBuilder` in [`store/`](../include/simulator/store/), linked as `simulator_store`; `StoreLock` in the server's [`itch_input.hpp`](../server/include/chronos/server/itch_input.hpp) |
| Day chain | `DaySequenceSource` ([`itch_input.hpp`](../server/include/chronos/server/itch_input.hpp)) |
| Historical book | `MarketBook` and `MarketBooks` ([`market/`](../include/simulator/market/)), over `OrderBook`, `Order` and `PriceLevel` ([`orders/`](../include/simulator/orders/)) |
| Book storage | `BookStorage`, `OrderPool`, `LevelPool`, `OrderIndex`, `LevelIndex` ([`orders/`](../include/simulator/orders/)) |
| Replay loop | `MarketReplay::run` ([`market/`](../include/simulator/market/)) |
| Decode/apply pipeline | `PipelineSource` and `SpscRing` ([`market/`](../include/simulator/market/)) |
| Event loop | `EventLoop<S>` ([`engine/`](../include/simulator/engine/)) |
| Simulator event queue | `SimQueue`, `SimEvent` ([`sim/`](../include/simulator/sim/)) |
| Order gateway | `OrderGateway` ([`sim/`](../include/simulator/sim/)) |
| Execution simulator | `ExecutionSimulator`, `ExecutionReport` ([`sim/`](../include/simulator/sim/)) |
| Portfolio | `Portfolio`, `RiskLimits`, `FeeSchedule` ([`portfolio/`](../include/simulator/portfolio/)) |
| Strategy boundary | the `Strategy` concept, `StrategyContext`, `NullStrategy` ([`engine/strategy.hpp`](../include/simulator/engine/strategy.hpp)); `MarketMaker`, `ImbalanceTaker`, `Orb` ([`strategies/`](../include/simulator/strategies/)) |
| Script strategies | `ScriptStrategy` (the sandbox and the Lua API), `describe_script`, `make_script_strategy` and `ScriptStore` ([`script_strategy.hpp`](../server/include/chronos/server/script_strategy.hpp)); Lua's fixed hash seed in [`cmake/lua_lstate.cpp`](../cmake/lua_lstate.cpp) |
| Strategy registry | `StrategyInfo`, `ParamInfo`, `AnyStrategy`, `register_plugin` and `resolve_params` ([`engine/strategy_registry.hpp`](../include/simulator/engine/strategy_registry.hpp)); one plugin file per strategy in [`strategies/`](../strategies/); `make_strategy` and `strategy_spec` in the server's [`session_strategy.hpp`](../server/include/chronos/server/session_strategy.hpp) |
| Day library | `DayLibrary` ([`itch_input.hpp`](../server/include/chronos/server/itch_input.hpp)) |
| Replay session | `ReplaySession` ([`replay_session.hpp`](../server/include/chronos/server/replay_session.hpp)), run by `SessionRunner`; one `Generation` at a time; `SessionStrategy`; the channels `FixedQueue`, `LatestSlot` and `RecordRing` ([`channels.hpp`](../server/include/chronos/server/channels.hpp)) |
| Command log writer | `LogQueue`, `LogWriter` ([`command_log.hpp`](../server/include/chronos/server/command_log.hpp)) |
| Strategy Lab | `Backtester` ([`lab.hpp`](../server/include/chronos/server/lab.hpp)), `backtest()` ([`backtest.hpp`](../server/include/chronos/server/backtest.hpp)) |
| HTTP and WebSocket I/O | `HttpServer`, `Peer` and `Hub`, the protocol's encoders and decoders, the security checks ([`server/include/chronos/server/`](../server/include/chronos/server/)) |
| Browser UI | [`web/src/`](../web/src/): `store.ts` (the terminal's state), `lab.ts` (the Lab's and the scripts'), one component per panel, `Strategies.tsx` for the script editor |

## Where each invariant is enforced

The invariants are defined in [architecture/README.md](architecture/README.md#the-invariants).

| Invariant | Enforced by | Checked by |
|---|---|---|
| I1 one event contract | every source implements `MarketEventSource` and emits `MarketEvent` | `SourceConformanceTest`, instantiated for the ITCH, CSV and in-memory sources |
| I2 format isolation | `market/`, `orders/`, `sim/`, `portfolio/` and `engine/` include no reader header | inspection; not in CI |
| I3 history has one writer | `MarketBook::apply` is the book's only mutator; the simulator, portfolio and strategies get `const` references; shadow orders live in `ExecutionSimulator` | `BacktestDeterminismTest`: a backtest leaves the history digest of a plain replay |
| I4 a historical event is not an order | `MarketBook::apply` has no matching; `ExecutionSimulator::on_market` only observes | `MarketBookTest`, `BookModelTest`; 0 rejections on the full day |
| I5 deterministic order | `EventLoop` merges by (timestamp, stream, sequence) and reads no clock; `ReplaySession` applies commands at boundaries | `EventLoopTest.TieGoesToMarketInTheSpecWalkthrough`, `ReplaySessionTest.OrderingAtOneTimestampIsMarketThenSimulatorThenCommand`, `CommandLogTest`, `LabTest`, `WatchTest` |
| I6 no look-ahead | `StrategyContext` holds the post-event book; `OrderGateway` schedules arrivals after the order latency | the event loop's latency tests |
| I7 the engine thread owns the engine | `ReplaySession`'s engine-only members; the admission queue, `LatestSlot` and `RecordRing` | `chronos_server_tests` under ThreadSanitizer in CI |
| I8 headless isolation | `simulator_lib` links only the standard library; the server is `chronos_server` | [`scripts/check-isolation.sh`](../scripts/check-isolation.sh) in CI: the engine's and the plugins' includes, and `simulator_lib`'s links |
| I9 bounded channels | fixed capacities in `ReplaySession` (1,024 commands, 64 controls, 65,536 records) and `LogQueue` (4,096) | `RecordRingTest`, `PeerTest`, `ServerTest.AClientThatStopsReadingNeverBlocksTheEngineOrOtherClients` |
| I10 no allocation on the replay path | `BookStorage` reserves pools, indices, ladder windows; the ring is sized before the replay | the counting `operator new` in [`app/alloc_counter.cpp`](../app/alloc_counter.cpp); [`scripts/check-hot-path.sh`](../scripts/check-hot-path.sh) in CI |
| I11 nothing applies without its place in the log | a command applies only with a slot `LogQueue::reserve` holds; `ReplaySession::admission` refuses while the log has failed | `PeerTest.ACommandWhoseRecordNeverReachesTheLogGetsLogUnavailable`, `CommandLogTest` |
| I12 measured claims only | [`bench/run.sh`](../bench/run.sh), [`bench/summary.py`](../bench/summary.py) | [`bench/RESULTS.md`](../bench/RESULTS.md) and `bench/results/` |
| I13 status is explicit | the status words of [architecture/README.md](architecture/README.md#status-words) | review |
| I14 behaviour is preserved | the full-day correctness mode of `bench/run.sh`; golden digests in the tests | `BacktestTest.TheFixtureRunsDigestsAreGolden`; the digests recorded in [`bench/RESULTS.md`](../bench/RESULTS.md) |

## Repository layout

| Path | Contents |
|---|---|
| `include/simulator/types/` | `Price`, `Quantity` (`uint64`), `Timestamp` (ns since the Unix epoch, UTC), `OrderId`, `SymbolId` (`uint32`), `Side` |
| `include/simulator/orders/` | `Order`, `PriceLevel`, `OrderBook` (a list or a ladder of levels), `OrderIndex` and `LevelIndex` (open addressing), `OrderPool`, `LevelPool`, and `BookStorage`, which reserves them all up front |
| `include/simulator/market/` | `MarketEvent`, `InstrumentRegistry`, `MarketEventSource`, `VectorSource`, `MarketBook`, `MarketBooks`, `MarketReplay`, `Trade`, and `SpscRing` and `PipelineSource` |
| `include/simulator/io/` | `MappedFile` |
| `include/simulator/itch/` | `ItchSource` with its framing (`ItchMessageReader`, `ByteReader`) and `ItchDecoder` |
| `include/simulator/csv/` | `CsvSource`, a CSV event source for the library and tests |
| `include/simulator/store/` | the per-symbol store's index and segment reader |
| `include/simulator/sim/` | `SimEvent` and `SimQueue`, `OrderGateway`, `ExecutionReport`, `ExecutionSimulator` |
| `include/simulator/portfolio/` | `Portfolio`, `RiskLimits`, `FeeSchedule` |
| `include/simulator/engine/` | `EventLoop<S>`, the `Strategy` concept, `StrategyContext`, `NullStrategy` |
| `include/simulator/strategies/` | `ImbalanceTaker`, `MarketMaker` and `Orb` (opening range breakout) |
| `strategies/` | the strategy plugins: one file each registers a strategy ([strategies.md](strategies.md)) |
| `include/simulator/metrics/` | `LatencyHistogram` |
| `include/simulator/cli/` | argument parsing and the usage text |
| `src/` | the implementations, mirroring `include/simulator/` |
| `app/` | `main.cpp`, and `alloc_counter.cpp`, a counting `operator new` linked only into executables |
| `server/` | `chronos-serve`: sessions, the protocol, the command log, the Lab, the store's builder |
| `web/` | the Trading Terminal and the Strategy Lab; Vitest unit tests and Playwright end-to-end tests |
| `bench/` | `simulator_bench.cpp`, `run.sh`, `summary.py`, [`RESULTS.md`](../bench/RESULTS.md) and `results/` |
| `tests/` | one suite per component; `tests/support/` holds the source conformance suite; `tests/itch/` a byte-exact ITCH builder checked against real NASDAQ bytes |
| `scripts/` | the format, isolation and hot-path checks CI runs, and the Docker entrypoint |

## Where to start reading

For a review, in this order:

1. [`include/simulator/market/market_event.hpp`](../include/simulator/market/market_event.hpp): the one event type
   every source emits.
2. [`include/simulator/market/market_book.hpp`](../include/simulator/market/market_book.hpp) and
   [`src/market/market_book.cpp`](../src/market/market_book.cpp): the L3 book, its invariants and digest.
3. [`include/simulator/orders/order_book.hpp`](../include/simulator/orders/order_book.hpp) and
   [`src/orders/order_book.cpp`](../src/orders/order_book.cpp): price levels as a list or a ladder a cent apart.
4. [`include/simulator/orders/book_storage.hpp`](../include/simulator/orders/book_storage.hpp): the pools and
   open-addressing indices that make the replay allocation-free.
5. [`include/simulator/market/spsc_ring.hpp`](../include/simulator/market/spsc_ring.hpp) and
   [`pipeline_source.hpp`](../include/simulator/market/pipeline_source.hpp): the two-thread pipeline.
6. [`include/simulator/engine/event_loop.hpp`](../include/simulator/engine/event_loop.hpp): the deterministic loop.
7. [`include/simulator/sim/execution_simulator.hpp`](../include/simulator/sim/execution_simulator.hpp) and its
   `.cpp`: the fill model.
8. [`include/simulator/strategies/orb.hpp`](../include/simulator/strategies/orb.hpp): a complete strategy against
   the `Strategy` concept.
