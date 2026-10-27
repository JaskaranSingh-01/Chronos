# Chronos documentation

| Document | Covers |
|---|---|
| [getting-started.md](getting-started.md) | Docker in one command, the native build, the data, first runs |
| [architecture.md](architecture.md) | the three parts, the data flow, the threads, the guarantees and where each is enforced, the repository layout, where to start reading |
| [market-data.md](market-data.md) | `MarketEvent`, the sources, ITCH 5.0 decoding, getting the data, the per-symbol store |
| [order-book.md](order-book.md) | the L3 book: applying events, allocation-free storage, the list and the ladder, invariants, digests |
| [backtesting.md](backtesting.md) | the deterministic event loop, the fill model, the portfolio and risk, the strategies (including the opening range breakout) |
| [performance.md](performance.md) | the throughput and latency numbers, the profile, the two-thread pipeline, the method, reproducing them |
| [server.md](server.md) | `chronos-serve`: sessions, the command log, the Strategy Lab, the HTTP API, the WebSocket protocol, security |
| [ui.md](ui.md) | the Trading Terminal and the Strategy Lab, how the page is built, its tests and its measurements |
| [cli.md](cli.md) | every option of `simulator` and `chronos-serve`, exit codes, `--json` |
| [testing.md](testing.md) | the test suites, how correctness is checked, sanitizers, CI, formatting |
| [limitations.md](limitations.md) | what the simulation, the engine and the server assume, and the roadmap |

[`bench/RESULTS.md`](../bench/RESULTS.md) holds every benchmark run's record.

**Reviewing the code?** Read [architecture.md](architecture.md), then its "Where to start reading" list: eight files,
from the event type to a complete strategy.

**Running it?** [getting-started.md](getting-started.md).
