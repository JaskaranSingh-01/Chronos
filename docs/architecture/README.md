# Chronos architecture

This folder is the design record of Chronos: what it is for, how its parts divide the work, the contracts between
them, the rules every change must keep, what is built and what is not. It is written to be read before the code. The
rest of [`docs/`](../README.md) is the reference: commands, options, protocol fields, numbers. Where a design document
needs a detail, it links there rather than repeating it.

## Reading order

| Document | Answers |
|---|---|
| [overview.md](overview.md) | what Chronos is for, who uses it, what it must demonstrate, scope, milestones and status |
| [system-architecture.md](system-architecture.md) | the components, their responsibilities and boundaries, the executables, the threads |
| [market-data-and-replay.md](market-data-and-replay.md) | from a recorded feed to normalized market events and the historical book |
| [event-and-execution-model.md](event-and-execution-model.md) | what each market event means, and how simulated orders are filled without touching history |
| [web-terminal.md](web-terminal.md) | `chronos-serve`, the engine thread, the channels, pacing, seeking and the browser |
| [performance-and-determinism.md](performance-and-determinism.md) | how speed is measured, the recorded baseline, digests and what they prove |
| [decisions.md](decisions.md) | the architecture decisions, each with its alternatives, consequences and status |
| [testing-and-acceptance.md](testing-and-acceptance.md) | how each layer is validated, and the acceptance criteria with their status |
| [glossary.md](glossary.md) | the terms these documents use, each in one sense |

[`docs/code-map.md`](../code-map.md) maps the design to the code: the repository layout, where each guarantee is
enforced and tested, and the files to read first.

## Status words

Every capability, guarantee and criterion in these documents carries one of these:

| Status | Means |
|---|---|
| **Verified** | built, and a named test, CI check or recorded benchmark shows it holds |
| **Implemented** | built and unit-tested; the stronger property claimed for it has no further evidence yet |
| **Partial** | part of it is built; the document names the missing part |
| **Planned** | on the roadmap and not built; nothing in the code relies on it |
| **Out of scope** | a deliberate non-goal |
| **Open** | a question the evidence does not settle yet; listed under [open questions](#open-questions-and-known-gaps) |
| **Followed** | a rule for changes, kept by review and practice rather than by an automatic check |

Decisions use their own: proposed, accepted, implemented, superseded ([decisions.md](decisions.md)).

## The invariants

These hold for every change. A change that must break one is an architecture decision: record it in
[decisions.md](decisions.md) first. The other documents refer to them by number.

| # | Invariant | Status |
|---|---|---|
| **I1** | **One event contract.** Every input format is translated, at the input boundary, into one normalized market event. Everything downstream (the book, the replay loops, the simulator, the server's sessions and Lab) sees only that event, never a wire format. The one exception is the server's day library, which frames each new day file to build the per-symbol store and reads its System Events for the day's session hours (`server/src/itch_input.cpp`). | Verified for the format readers: the conformance suite runs over the ITCH, CSV and in-memory sources; the day chain and the two-thread pipeline are checked by their own equivalence tests |
| **I2** | **Format isolation.** Framing, message layouts, symbol directories and clock conversions belong to the format's reader (and, for a day's session hours, to the server's day library). The replay, book and simulation layers include no reader. | Implemented: true of the engine's includes today, by inspection; not checked in CI |
| **I3** | **History has one writer.** The historical book is changed only by applying market events from the recorded stream. Simulated orders never enter it, and historical orders are never matched against each other or against simulated ones. | Verified: a backtest leaves the same book digest as a plain replay |
| **I4** | **A historical event is not an order.** An add records that an order rested; it is never matched. An execution, cancel, delete or replace is applied to the order it names, with its exchange meaning ([event-and-execution-model.md](event-and-execution-model.md)). | Verified on NASDAQ's full day of 2019-12-30: 0 rejected events |
| **I5** | **Deterministic order.** Everything in a run is ordered by (timestamp, class, sequence), where the class is 0 for a market event, 1 for a simulator event and 2 for a manual command: market events first at equal times, then simulator events, then manual commands. The one exception is a command batch applied after a Step that stopped inside a timestamp: it applies before that timestamp's remaining market events, and the simulator events it creates still wait for them. No result depends on a wall clock, except that a script call that runs past 30 s of wall time fails its run ([D22](decisions.md#d22-strategies-written-in-the-browser-are-sandboxed-lua-in-the-server)). The same input, configuration, build and commands give the same digests. | Verified: repeated runs, command-log reproduction and Lab verification give equal digests |
| **I6** | **No look-ahead.** A strategy sees a market event only after the historical book has applied it. Its orders reach the simulated exchange no earlier than the order latency later, and its reports no earlier than the report latency after they happen. | Verified by worked-example tests of the loop |
| **I7** | **The engine thread owns the engine.** A session's book, simulator, portfolio and strategy are read and written by one thread. Other threads reach them only through the admission queues, the published snapshot, the chart-history slot and the report ring, and read the session's state, generation and seek position as atomics. | Implemented; the channels run under ThreadSanitizer in CI |
| **I8** | **Headless isolation.** The engine library and the headless executables (`simulator`, the benchmarks) include and link no server, network, JSON or UI code. | Verified for the engine's sources, the strategy plugins and the library by `scripts/check-isolation.sh` in CI; the executables' link lines (the CLI adds the store's zstd and xxHash, never the server) by inspection |
| **I9** | **Bounded channels.** Every queue between the server and the engine has a fixed capacity and a defined behaviour when full; none grows inside the engine. | Partial: see Q4 and Q12 |
| **I10** | **No allocation on the replay path.** All storage a replay needs is reserved before it starts. A full structure stops the run with a named cause; nothing grows mid-replay. | Verified: 0 allocations over the full day, counted; a hot-path check in CI |
| **I11** | **Nothing applies without its place in the log.** An interactive session applies a command only after the command log has reserved room for its record; once the log fails (its queue full, or a write failed), commands are refused. Records still queued when a write fails are lost, so the log reproduces the session up to the failure. | Verified: the command-log and peer tests refuse a command the log cannot hold |
| **I12** | **Measured claims only.** A performance claim names its configuration, workload, machine and method, and comes from a recorded run ([performance-and-determinism.md](performance-and-determinism.md)). | Followed for every number in these documents |
| **I13** | **Status is explicit.** Planned, partial, implemented and verified are kept apart in every document, the README included. | Followed in these documents |
| **I14** | **Behaviour is preserved unless a change says otherwise.** A change not meant to alter behaviour leaves the full-day replay digest and the backtest digests unchanged; one that is meant to is recorded with its new digests. | Followed by every engine round recorded in `bench/RESULTS.md` |

## Open questions and known gaps

Collected here once; each document links to its entries.

| # | Question or gap | Evidence | Where it matters |
|---|---|---|---|
| Q1 | **Is the fill model "conservative"?** The README says so. Queue position is conservative (an order joins the back), but fill rule (c), which fills a resting order when a historical opposite-side order is added at or through its price, assumes that participant would have traded with us. It produced 106 of the 143 fills (102 from adds, 4 from replaces) in the published SPY market-maker run on the first 256 MiB of the 2019-12-30 day file, mostly pre-market. No run measures fills and PnL with the rule turned off. | [backtesting.md](../backtesting.md#results-on-real-data) | [event-and-execution-model.md](event-and-execution-model.md#the-fill-rules) |
| Q2 | **The full-day digest does not prove prices.** Every book is empty at the close, so the combined digest fingerprints each symbol's adds and replaces. A decoder that misread prices could print the same digest. A rolling digest over every event's resulting top of book, or an external reference (LOBSTER), would close this. | [order-book.md](../order-book.md#digests) | [performance-and-determinism.md](performance-and-determinism.md#what-the-digests-prove) |
| Q3 | **The engine thread can wait briefly.** The UI design said the engine never waits on the report ring. While it works, the engine only try-locks the admission queues, the snapshot slot and the chart-history slot. But between ticks its runner takes the admission lock with a blocking lock to sleep on it, and an I/O thread holds that lock while it numbers a command and publishes its Received and Admitted records; and the engine shares three other locks: the record publisher's (an I/O thread holds it while it records an admitted command), the report ring's (a reader holds it while copying at most 4,096 records) and the log queue's (the writer holds it while it takes a record). Each is held for a short, bounded copy, never across network or disk I/O; no measurement of these waits is recorded. | the report ring's own comment; the session's admission | [web-terminal.md](web-terminal.md#the-channels) |
| Q4 | **A client that stops reading grows the server's memory.** Its WebSocket send buffer holds the snapshots it does not read, about 1 MB a second, until it reads or disconnects. The engine is not affected; the server process is. | [limitations.md](../limitations.md#what-the-server-assumes) | I9 |
| Q5 | **Two replay loops.** The headless multi-symbol replay has its own loop; backtests and sessions use the event loop. Both consume the same events and the same book, but the plan to run the replay through the event loop with a null strategy waits on a measurement that its throughput matches. | [limitations.md](../limitations.md#roadmap) | [D3](decisions.md#d3-one-deterministic-step-wise-event-loop) |
| Q6 | **ITCH coverage.** Cross trades (`Q`), broken trades (`B`), operational halts (`h`) and market-wide circuit breakers (`W`) are not decoded, and the printable flag of an execution is dropped. The book is unaffected; the candles, the tape and the opening range include non-printable executions and miss the crosses' prints. | [limitations.md](../limitations.md) | [market-data-and-replay.md](market-data-and-replay.md#messages-not-translated) |
| Q7 | **The UI's effect on headless results** was gated once, when the event loop became step-wise (commit `05623c2`: backtest outputs identical, loop benchmark within the before spread). No full-day before-and-after matrix for the server's addition is in `bench/RESULTS.md`; the replay path itself does not run through the code the UI changed. A 1 % tolerance has not been adopted; the design's criterion is "within run-to-run spread". | the commit message of `05623c2` | [performance-and-determinism.md](performance-and-determinism.md#headless-isolation) |
| Q8 | **Per-event latency is a batch estimate.** The percentiles are of 1,024-event batch means, as the clock cannot time one event on the measurement machine. True per-event tails need a finer clock (e.g. a cycle counter on x86). | [performance.md](../performance.md#method) | [performance-and-determinism.md](performance-and-determinism.md#what-is-measured) |
| Q9 | **The Linux heap check counts no blocks.** On glibc the "heap blocks unchanged" figure is always 0; the heap byte count is still compared. | the allocation counter's Linux branch | [performance-and-determinism.md](performance-and-determinism.md#allocations) |
| Q10 | **Some trading hours are fixed** at 09:30-16:00 ET. The server reads each day's own session from its System Events, but only for a strategy's trading window and the sessions a strategy is built with; the CLI's session, the crossed-book check and the fill attribution's phases (the Lab's included) use 09:30-16:00, so half days are wrong there. | [limitations.md](../limitations.md#what-the-engine-assumes) | [market-data-and-replay.md](market-data-and-replay.md#time) |
| Q11 | **An order in flight at a day boundary** rests on the next day; resting orders expire at the boundary as day orders do. | the execution simulator's comment on Clear | [event-and-execution-model.md](event-and-execution-model.md#day-boundaries) |
| Q12 | **A record's size is not fixed.** The channels' slot counts are fixed, but a record of an attach carries the strategy's spec, with a script's source of up to 48 KiB, so the report ring's memory is bounded by 65,536 × 48 KiB, about 3 GiB, not by its 47 MB of fixed fields. Each attach publishes three records, and a connection may send 50 commands a second. | `channels.hpp`'s comment on `FixedQueue`; the session's `ring_capacity` | I9, [web-terminal.md](web-terminal.md#the-channels) |
