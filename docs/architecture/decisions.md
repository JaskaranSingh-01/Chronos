# Architecture decisions

Each decision records its status, the problem, the choice, the alternatives that were recorded, the reasons, the
consequences, what a future change must preserve, and the evidence still missing. Statuses:

| Status | Means |
|---|---|
| **proposed** | written down, not agreed; nothing relies on it |
| **accepted** | agreed in an approved design; not (fully) built |
| **implemented** | agreed and built; the code relies on it |
| **superseded** | replaced by a later decision, named |

The approved designs these come from are the project's design specifications (architecture, 2026-10-09, with the
event contract revision of 2026-10-12; UI, 2026-10-15; Strategy Lab, 2026-10-18; per-symbol store, 2026-10-21; engine
performance, 2026-10-24; ORB, 2026-10-27), kept outside this repository. This file is the record that ships with the
code. D1 to D4 keep the numbers the architecture specification gave them, which [backtesting.md](../backtesting.md)
also uses; the rest are numbered here. Where an "Alternatives" entry says nothing was recorded, none was.

| # | Decision | Status |
|---|---|---|
| [D1](#d1-historical-reconstruction-and-simulated-execution-are-separate-paths) | Historical reconstruction and simulated execution are separate paths | implemented |
| [D2](#d2-the-fill-model) | The fill model | implemented; its sensitivity is unmeasured |
| [D3](#d3-one-deterministic-step-wise-event-loop) | One deterministic, step-wise event loop | implemented; running the replay through it is accepted, not built |
| [D4](#d4-one-event-contract-and-one-book) | One event contract and one book | implemented |
| [D5](#d5-format-specific-readers-normalize-at-the-input-boundary) | Format-specific readers normalize at the input boundary | implemented |
| [D6](#d6-the-strategy-boundary) | The strategy boundary | implemented |
| [D7](#d7-the-order-book-representation) | The order-book representation | implemented; a bitmap over the ladder is proposed |
| [D8](#d8-order-storage-preallocated-fixed-capacity) | Order storage: preallocated, fixed capacity | implemented |
| [D9](#d9-a-separate-chronos-serve-executable) | A separate `chronos-serve` executable | implemented |
| [D10](#d10-the-engine-thread-owns-the-session-channels-are-bounded) | The engine thread owns the session; channels are bounded | implemented, with two known gaps |
| [D11](#d11-determinism-digests-and-reproduction) | Determinism, digests and reproduction | implemented; a rolling digest is proposed |
| [D12](#d12-benchmark-isolation-and-the-evidence-rule) | Benchmark isolation and the evidence rule | implemented |
| [D13](#d13-symbol-and-day-scope) | Symbol and day scope | implemented |
| [D14](#d14-a-single-threaded-engine-with-a-two-thread-replay-pipeline) | A single-threaded engine, with a two-thread replay pipeline | implemented; supersedes "no multi-threading" |
| [D15](#d15-integer-prices-per-instrument-scales-epoch-nanoseconds) | Integer prices, per-instrument scales, epoch nanoseconds | implemented |
| [D16](#d16-the-per-symbol-store) | The per-symbol store | implemented |
| [D17](#d17-the-simulation-models-simplifications) | The simulation model's simplifications | implemented |
| [D18](#d18-backward-seek-by-restart-and-fast-forward) | Backward seek by restart and fast-forward | implemented |
| [D19](#d19-a-second-real-source-lobster) | A second real source: LOBSTER | accepted, not built |
| [D20](#d20-retire-the-pre-m1-pipeline-and-its-matching-engine) | Retire the pre-M1 pipeline and its matching engine | implemented |

---

## D1. Historical reconstruction and simulated execution are separate paths

**Status:** implemented (the architecture specification's hard separation rule).

**Context.** A simulator that inserts its own orders into the replayed book, or matches historical orders against each
other, produces a history that never happened, and every later event is applied to the wrong state.

**Decision.** Historical replay and simulated execution share no mutable state. The historical book has one writer:
the market stream. Historical orders are never matched against each other. Simulated orders live only in the
execution simulator, as shadow orders with a recorded place in the historical queue, and are never inserted into the
book. Strategies, the simulator and the portfolio get read-only views of the book. Chronos therefore has no matching
engine for historical orders; the execution simulator decides only simulated fills.

**Alternatives.** The first version's synthetic exchange, which matched orders against each other; the architecture
kept it isolated from the historical book, then retired it ([D20](#d20-retire-the-pre-m1-pipeline-and-its-matching-engine)).

**Consequences.** The book cannot model market impact ([D17](#d17-the-simulation-models-simplifications)). A
simulated order's fills come from rules over historical events, which must be stated and tested ([D2](#d2-the-fill-model)).

**Preserve.** [I3](README.md#the-invariants), [I4](README.md#the-invariants): a backtest leaves the same book digest
as a plain replay.

## D2. The fill model

**Status:** implemented. Its sensitivity is unmeasured.

**Context.** Fills decide every execution-sensitive result. The model must be explicit and testable, and conservative
where the data allows.

**Decision.** Day orders are post-only and rest at the back of their price's visible queue. On each market event,
before the book applies it, four rules decide fills: (b) removal of historical quantity ahead shrinks the queue ahead;
(a) an execution behind fills us; (d) a same-side execution at a worse price fills us; (c) an opposite-side add or
replace at or through our price fills us. A trade-price guard stops (a) and (d) when the execution was at a better
price than ours. IOC orders sweep the visible book on arrival without removing liquidity. Off-grid prices are rejected.
A FIFO-anomaly counter checks the rules on real data
([event-and-execution-model.md](event-and-execution-model.md#the-fill-rules)).

**Alternatives.** The first design had rules (a) to (c) only. Without (d), an execution at a price worse than ours did
not fill us, which biases passive fill rates down, so (d) was added. Executing the marketable part of a day order and
resting the rest was considered; a marketable day order is rejected instead (post-only).

**Consequences.** Rule (c) is an assumption about counterparties, not a consequence of FIFO
([Q1](README.md#open-questions-and-known-gaps)). Aggressive fills are optimistic without market impact.

**Preserve.** Every rule is pinned by a worked-example test; a rule change is a decision, with new digests.

**Evidence still required.** Fill rate and PnL with rule (c) off, and across latencies, on real days.

## D3. One deterministic, step-wise event loop

**Status:** implemented (the loop, and its step-wise form in commit `05623c2`). Running the headless replay through it
with a null strategy is accepted (task M3.07) and not built.

**Context.** A backtest needs one order of events that never depends on a clock. The original loop then pulled the
next market event before running due simulator events, so nothing outside it could advance time between market events:
an interactive session could not pace, pause or deliver an order on a quiet symbol.

**Decision.** One loop merges the market stream with the simulator's own events in (exchange time, class, sequence)
order, market first at a tie, and reads no clock. It exposes peek, advance-to, step and finish, and its run is built
from them, so a headless backtest and a paced session run the same code. Simulator events at a market event's exact
time run after every market event at that time.

**Alternatives.** A wrapper around the source (cannot advance simulator time between market events); a hook called
while waiting for the next market event, in the UI design's first version (rejected: it delayed order arrivals and
fills on quiet symbols until the next market event).

**Consequences.** One path to test; the server adds pacing around it, not inside it. The headless multi-symbol replay
still has its own loop ([Q5](README.md#open-questions-and-known-gaps)).

**Preserve.** The ordering contract ([I5](README.md#the-invariants)); the same end-of-stream policy headless and
interactive.

**Evidence still required.** For running the replay through it: a measurement that it matches the replay loop's
full-day throughput.

## D4. One event contract and one book

**Status:** implemented (the event contract revision of 2026-10-12).

**Context.** The first version replayed through a "replay event" type and a replay engine shaped by its first inputs,
with a reader per format and a simulator class that both replayed history and matched orders. Each new input bent the
shared types further.

**Decision.** One normalized market event type, one source interface (pull one event at a time, end, or error with a
position), one historical book, and a conformance suite every format reader must pass. Every consumer (the replay
loop, the event loop, the server, the Lab) takes that event.

**Alternatives.** Keeping the first version's types and engine, extended per format; replaced.

**Consequences.** The event is fixed-size and must carry every format's needs (a replace's new id, an execution's own
price, a trading state).

**Preserve.** [I1](README.md#the-invariants). No consumer may depend on a format's details.

## D5. Format-specific readers normalize at the input boundary

**Status:** implemented.

**Context.** ITCH needs framing, a locate-to-symbol map, instrument registration from directory messages, a clock
conversion from nanoseconds after midnight, and fast skipping of unselected symbols. None of this concerns the book.

**Decision.** All of it lives in the reader. Readers validate the wire format and emit events in the contract's units;
the book validates state. Errors are sticky and carry a position.

**Alternatives.** The first version's separate normalizer stage between the ITCH decoder and the replay; retired, the
reader now builds events itself.

**Consequences.** Format optimisations, such as filtering by locate before decoding or decoding on a thread of its own,
change no downstream code.

**Preserve.** [I2](README.md#the-invariants). A new format passes the conformance suite before anything else uses it.

## D6. The strategy boundary

**Status:** implemented.

**Decision.** A strategy is a type with three callbacks (an applied market event, a delivered report, a timer), each
given a context holding the read-only book, the read-only portfolio, the order gateway and the trade the event
printed. It is a compile-time concept. The strategy registry ([D21](#d21-strategies-are-plugins-behind-a-registry))
wraps each strategy behind a virtual interface, and the server holds one, or none, in an adapter it can swap at a
command boundary.

**Alternatives.** The UI design allowed virtual dispatch in the server's adapter, as cheap enough at UI speed. The first
implementation used a closed set of types instead, which D21 replaced.

**Consequences.** A strategy cannot change history, read a wall clock, or see an event before the book has applied it
([I6](README.md#the-invariants)). Adding a strategy means adding one file ([D21](#d21-strategies-are-plugins-behind-a-registry)). Manual orders
from the terminal do not pass through the strategy interface: there is no "UI bridge" strategy. They go through the
same order gateway, so they meet the same risk checks, latency and fill rules, and the command log records them as
commands with their own lifecycle.

**Preserve.** Strategies get read-only state and act only through the gateway.

## D7. The order-book representation

**Status:** implemented. A bitmap over the ladder's slots is proposed.

**Decision.** A full L3 book: each order in FIFO order within its price level, with an arrival sequence number. Orders
and levels live in shared pools, found through open-addressing indices. Each side's levels form a list, best first;
by default a window of 256 cent-wide slots around each side's best speeds up placing a new level near the best.

**Alternatives.** Node-based ordered maps per side, the previous engine; replaced, and measured against, for their
allocations and cache behaviour. The ladder with one-price-unit slots was built first; it held about one cent level
per side, missed most new levels, and was widened to a cent after review. A sorted array per side has not been
evaluated.

**Consequences.** Placing a new level (the ladder's scan of its window, then the list's walk for a price off the grid
or outside the window) is still 40 % of the book thread's time. Two backends must be kept equivalent; a model test
compares both with a reference after every operation.

**Preserve.** Both backends give the same book and digest on the full day.

**Evidence still required.** For the bitmap: the full-day matrix against the current default.

## D8. Order storage: preallocated, fixed capacity

**Status:** implemented.

**Decision.** Every structure a replay needs is reserved before it starts, from one capacity parameter. A full
structure stops the run with its name rather than growing.

**Alternatives.** Growing containers, the previous engine: 43.9 million allocations over the full day.

**Consequences.** A run must be sized; the default for every symbol of a day is 4,194,304 orders against a peak of
1,924,078. Reserved memory is larger than the previous engine's peak (about 1.06 GB reserved on the ladder).

**Preserve.** [I10](README.md#the-invariants); the CI hot-path check.

## D9. A separate `chronos-serve` executable

**Status:** implemented (UI design, 2026-10-15).

**Context.** An interactive UI needs HTTP, WebSocket, JSON and threads that the engine and its benchmarks must not
carry.

**Decision.** The server is a library and executable of its own on top of the engine. The engine library links only
the standard library; CI checks that nothing in the engine includes server or web code.

**Alternatives.** The design records alternatives for the libraries only: Crow with Asio (against CivetWeb) and React
(against Svelte). Keeping the engine free of the server was a requirement from the start.

**Preserve.** [I8](README.md#the-invariants).

## D10. The engine thread owns the session; channels are bounded

**Status:** implemented, with two known gaps.

**Decision.** One engine thread per session owns its book, simulator, portfolio and strategy. The I/O threads reach it
only through bounded admission queues (commands and controls, refused when full), a latest-value snapshot slot (the
engine skips a frame rather than wait), a numbered report ring (readers that fall behind resync), and a bounded log
queue. The engine only try-locks the admission queues and the snapshot slot; the other locks it shares are held by
other threads for short, bounded copies, never across network or disk I/O. Queues, slots and rings are allocated at
session start.

**Alternatives.** A lock-free triple buffer for snapshots, deferred: a mutex slot the engine only try-locks was the
simple, proven option for the first version, to be replaced only if a measurement shows the engine waiting.

**Consequences.** A slow client loses snapshots, not correctness. Two gaps: the engine can wait briefly on a lock
another thread holds ([Q3](README.md#open-questions-and-known-gaps)), and a client that stops reading grows its own
send buffer in the server ([Q4](README.md#open-questions-and-known-gaps)).

**Preserve.** [I7](README.md#the-invariants), [I9](README.md#the-invariants).

**Evidence still required.** A measurement of the engine's waits under a slow reader and heavy admission; a bound on
a client's send buffer.

## D11. Determinism, digests and reproduction

**Status:** implemented. A rolling digest is proposed.

**Decision.** No wall clock in any result; explicit ordering; integer arithmetic. Digests of the book, the execution
state and the portfolio, the last covering every fill in order. Every interactive session writes a command log with
the build and each input's hash; every Lab run records its build, inputs and settings, and can be verified or watched
again to the same digests, or is reported as run on different code or changed input.

**Alternatives.** The Lab's first portfolio digest hashed the totals only; it was replaced (commit `f45ac6a`) by one
over every fill in order, since two different fill sequences can share their totals.

**Consequences.** The end-of-day replay digest is a regression check, not a proof of prices
([Q2](README.md#open-questions-and-known-gaps)).

**Evidence still required.** For the rolling digest: its cost on the full day.

## D12. Benchmark isolation and the evidence rule

**Status:** implemented.

**Decision.** Benchmarks build the engine without the server; per-event timing is a separate loop used only when asked
for. Every performance claim comes from interleaved, repeated full-day runs with a recorded method, after an exact
correctness gate; defaults are chosen by a written selection rule.

**Consequences.** Claims are slower to make, and narrower (one machine). See
[performance-and-determinism.md](performance-and-determinism.md#rules-for-a-performance-claim).

## D13. Symbol and day scope

**Status:** implemented.

**Decision.** A replay covers one symbol, a list or all symbols of one day, with one book per symbol over shared
storage. A backtest trades one symbol: in the CLI over one day, in the server (the terminal and the Lab) over one or
more consecutive days played as one stream with a Clear between days. Several venues, or several traded symbols in one
run, are out of scope; a time-ordered merge of sources is planned for multi-venue replay.

**Consequences.** The portfolio is single-symbol. Cross-symbol strategies are not possible.

## D14. A single-threaded engine, with a two-thread replay pipeline

**Status:** implemented. Supersedes the first architecture's non-goal "multi-threading".

**Decision.** The engine's state is owned by one thread. For the headless replay only, a reader thread decodes ahead
into a single-producer single-consumer ring and the book thread applies in source order, giving the same books.
Sessions and backtests stay on one thread each.

**Rationale.** Measured: +44 % throughput on the full day for +37 % CPU, with identical digests
([performance-and-determinism.md](performance-and-determinism.md#the-recorded-baseline)).

**Preserve.** The two-thread output equals the one-thread output.

## D15. Integer prices, per-instrument scales, epoch nanoseconds

**Status:** implemented (the event contract revision of 2026-10-12).

**Decision.** Times are nanoseconds since the Unix epoch, UTC; each source converts its own clock. Prices are integer
ticks at a per-instrument price scale; quantities 64-bit integers at a per-instrument quantity scale; money integer
with 128-bit products.

**Consequences.** Multi-day runs and other venues need no change of units; no rounding enters a result.

## D16. The per-symbol store

**Status:** implemented (2026-10-24).

**Decision.** The server builds each new day once into the store: one compressed file per symbol, holding a segment
per day, each segment hash-checked as it is read. Builds commit atomically and are crash-safe; one server holds the
store's lock. Every one-symbol replay reads its segments, and Lab runs record each segment's hash as the day's
identity.

**Alternatives.** A faster full scan (rejected: the disk's floor is 15 to 27 s a day); an index of offsets into the
day file (rejected: a busy symbol's messages sit on almost every page, so it would still read the whole day);
re-sorting the day files by symbol (rejected: breaks the all-symbol replay, the benchmarks and the published digest).

**Consequences.** The day files stay the source of truth and the store is a derived, rebuildable cache: the
all-symbol replay and the benchmarks read the day files unchanged. The store takes about a third of the data's size
on disk; a build needs free space of 60 % of its day file.

## D17. The simulation model's simplifications

**Status:** implemented, documented as limitations.

**Decision.** No market impact; constant order and report latencies, the order latency also standing for the
market-data delay; IOC and post-only day orders (and protected market orders in the terminal); per-share fees; no
auction model; hidden liquidity never fills a simulated order.

**Rationale.** Each keeps the model exact and testable, and each is stated with the direction of its bias
([event-and-execution-model.md](event-and-execution-model.md#what-the-model-leaves-out)).

**Evidence still required.** Latency sensitivity runs; regulatory sell fees (planned).

## D18. Backward seek by restart and fast-forward

**Status:** implemented.

**Decision.** A backward seek starts a new generation over the same days and replays at full speed to the target.
Commands for an older generation are refused. Checkpoints for an instant backward seek are left possible by the
protocol and not built.

**Consequences.** A backward seek costs a replay from the start of the chain (fast for one symbol from the store); its
result is exactly a fresh run's.

## D19. A second real source: LOBSTER

**Status:** accepted in the first milestone's plan; not built.

**Decision.** A LOBSTER reader, held to the conformance suite, and a per-message comparison of the rebuilt book with
LOBSTER's own book snapshots.

**Rationale.** It would be the independent correctness reference the project lacks
([Q2](README.md#open-questions-and-known-gaps)).

## D20. Retire the pre-M1 pipeline and its matching engine

**Status:** implemented (2026-10-13).

**Context.** The first code had a simulator with a synthetic matching engine and a format-specific replay engine. The
architecture kept the matching engine isolated, never connected to the historical book, until a decision was made.

**Decision.** Removed, with its tests. The execution simulator ([D2](#d2-the-fill-model)) is the only place simulated
fills are decided.

## D21. Strategies are plugins behind a registry

**Status:** implemented (2026-10-29).

**Context.** The user means to write several strategies. Adding the ORB had touched ten files outside the strategy
itself: the CLI's flags, parsing, printing and JSON; the server's strategy enum, decoder, encoder, command log, default
limits and results; the Lab's types. Every new strategy would repeat that.

**Decision.** A strategy is a plugin: one file in `strategies/` that registers it with its name, label, summary,
params (each a field of its params struct, with bounds and help), default risk limits, and optionally how to build it,
its stats and a check of its params against the limits. The registry is the only list of strategies: the CLI's
`--strategy` and `--param`, the server's decoder and `/api/strategies`, the command log, the Lab's results and the
Lab's form all read it. A strategy runs behind `AnyStrategy`, whose callbacks are virtual. Every plugin is compiled
into an object library linked into each executable, so its registration runs at start-up.

**Alternatives.**

- Keep the closed set of types: static dispatch, but each strategy edits the CLI, the server and the UI.
- Load compiled strategies at run time from shared libraries: no rebuild, but an ABI to keep stable, a crashing
  plugin taking the server down, and a saved run that cannot prove which plugin binary it ran.
- Generate a variant of every plugin at build time: static dispatch with no edits, at the cost of code generation in
  CMake; not needed while the virtual call costs nothing measurable.

**Consequences.** One virtual call per callback: on SPY's full day the market maker's median went from 109.8 to
108.4 ns an event and the imbalance taker's from 93.1 to 95.0, within the spread of 11 alternating runs, with every
fill, PnL and book unchanged (the record is outside the repository, with the overnight work). A run's spec and the
command log carry a strategy's name and every param, sorted by name; the log's protocol went to 4, and a log of
protocol 3 is refused as written by another build. The CLI's strategy-specific flags became `--param NAME=VALUE`, and
the market maker's name is `market_maker` everywhere. A new strategy needs a rebuild and a server restart.
`StrategyRegistryTest` runs every plugin, so a new one is checked for determinism and for leaving history alone with
no test of its own.

**Preserve.** Nothing outside a plugin names a strategy. A plugin includes only the engine
(`scripts/check-isolation.sh` scans `strategies/`).
