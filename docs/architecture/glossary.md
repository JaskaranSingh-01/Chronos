# Glossary

Each term is used in one sense across these documents. Where two terms are easy to confuse, the entry says how they
differ.

## Data and replay

**Market-data message.** One message of a recorded feed in its wire format, such as an ITCH `A` (add order) or `E`
(order executed). Only a format reader sees messages.

**Market event.** The normalized form of a market-data message that changes or describes the historical market: an
add, execute, cancel, delete, replace, trade, status or clear, with its time, symbol, order ids, side, price and
quantity in the contract's units. Every reader emits market events, and nothing downstream sees anything else. Also
called a **historical event**. (The first version's "replay event" type is retired:
[D4](decisions.md#d4-one-event-contract-and-one-book).)

**Format reader** (or **source**). The component that turns one input format into market events, one at a time, ending
with an end of stream or an error that carries its position.

**Instrument registry.** The table of symbols seen so far: each symbol's dense id and its price and quantity scales.

**Historical replay.** Applying a recorded feed's market events, in order, to rebuild the historical books, with no
simulation. Its output is the books, the counts of events by type and result, invariant checks and digests.

**Order-book reconstruction.** Rebuilding the historical L3 book of a symbol from its market events: every displayed
order, in time priority within its price level.

**Historical book** (or **L3 book**). The reconstructed book of one symbol. It has one writer, the market stream, and
is read-only to everything else.

**Rejection.** A market event the book does not apply, because it does not fit the book's state (an unknown order, a
duplicate id, an over-execution). The book is unchanged, the rejection is counted, and the run continues. Different
from a **failure**, which stops the run: a reader error, an out-of-order time, exhausted capacity or a broken
invariant.

**Generation.** In the server, one replay of one symbol over its days from the start: a fresh book, simulator,
portfolio and strategy. A restart, a backward seek, a symbol load or a Watch starts a new one.

**Day chain.** Several consecutive days of one symbol played as one stream, with a clear between days.

**Per-symbol store.** The server's derived copy of each day, one compressed file per symbol, so one symbol's day can be
read without reading the whole day file.

## Trading and execution

**Historical execution.** An execute event in the recorded feed: some quantity of a historical order traded in the
real market. It reduces that order in the historical book. It is not a simulated fill.

<a id="trade"></a>**Trade** (or **market trade**, **print**). A trade printed by the historical market: a historical
execution at its price, or a trade event against hidden liquidity. Trades feed the candles, the tape and the trade a
strategy sees. A simulated fill is never a trade in this sense; it is drawn on the chart, not counted in it.

**Strategy order** (or **simulated order**, **our order**). An order sent by a strategy, or by a person at the
terminal, through the order gateway. It exists only in the simulation and never enters the historical book.

**Shadow order.** A simulated day order resting in the execution simulator: its price, remaining quantity, and its
place in the historical queue (the quantity ahead of it).

**Ahead quantity.** The historical quantity at a shadow order's price that was there when it arrived and has not yet
traded or left. It falls as those orders execute or leave.

**Matching engine.** A component that matches orders against each other to produce trades, as an exchange does.
Chronos has none for historical orders: the historical book only records what the feed says happened. The closest
thing is the execution simulator, which decides only simulated fills.

**Strategy plugin.** A file in `strategies/` that registers one strategy: its name, params, default risk limits and
stats. Built into every executable; nothing else names a strategy.

**Script** (or **strategy script**). A strategy written in the browser: Lua source saved by the server and run in a
sandbox; its strategy name is `script:` and its own name. A run keeps a copy of its source.

**Strategy registry.** The one list of strategies, sorted by name, from which the CLI, the server and the Lab learn
each strategy's params and build it.

**Execution simulator.** The component that holds shadow orders and decides their fills by the fill rules as market
events arrive, fills IOC orders against the visible book on arrival, and emits execution reports.

**Fill rule.** One of four rules, (a) to (d), by which a market event fills a shadow order, with a trade-price guard on
(a) and (d) ([event-and-execution-model.md](event-and-execution-model.md#the-fill-rules)).

**Simulated fill** (or **fill**). The execution of some or all of a simulated order's quantity, decided by the
execution simulator: at its own price as maker (a resting order filled by a fill rule) or at the book's prices as taker
(an IOC sweep). A fill changes the portfolio, never the historical book.

**Partial fill.** A simulated fill of less than the order's remaining quantity; the rest keeps resting (a day order)
or expires (an IOC). Different from a **partial execution**, which is a historical execution that leaves part of a
historical order in the book.

**Execution report.** The record of what happened to a simulated order: accepted, rejected with a reason, fill,
cancelled, cancel rejected or expired. Reports are delivered after the report latency.

**Order gateway.** The only way an order or cancel enters the simulation: it checks risk limits, assigns the id and
schedules the arrival after the order latency.

**Order latency, report latency.** The two constant delays of the model: from a strategy's decision to its order's
arrival (also standing in for the market-data delay), and from an outcome to its report's delivery.

**Portfolio.** The account of one run: position, cash, realised and unrealised PnL, fees, drawdown, and the risk limits
checked before each order.

**FIFO anomaly.** A fill by rule (a) or (d) while the shadow order still had quantity ahead of it, which a FIFO queue
would not allow; counted as a check on the model.

## Time and control

**Exchange time** (or **simulated time**, **market time**). The time of the replayed market, from market events and
the simulator's own scheduled events. All ordering, latencies and records use it.

**Wall-clock time.** The real time of the machine. The server's engine thread reads it to pace an interactive replay,
its I/O threads for timeouts and rate limits, and the replay loop to sample the cost per event when asked; it never
affects a result.

**Event loop.** The single-threaded loop that merges one symbol's market events with the simulator's events (order and
cancel arrivals, report deliveries, timers) in (exchange time, class, sequence) order, and drives the simulator, the
book, the portfolio and the strategy. It can run to the end or be stepped.

**Replay loop.** The headless loop that applies market events to one or many books with no simulation.

**Boundary.** A point between steps of the event loop where the server applies controls and commands. It is identified
by the market events applied so far, the exchange time and the command batch number.

**Command.** A client request that changes simulated trading: an order, a cancel, attaching or detaching a strategy.
It is admitted, numbered, logged and applied at a boundary.

**Control.** A client request that changes the replay: play, pause, step, speed, seek, cancel seek, restart, load a
symbol, watch a Lab run.

**Command queue** (and **control queue**). The bounded queues through which commands and controls enter the engine
thread. A full queue refuses; nothing is dropped silently.

**Command log.** The file every interactive session writes: a header with the build and each input's hash, then every
command and its lifecycle at its boundary. Replaying it reproduces the session.

**Snapshot.** A consistent picture of a session at one boundary, built by the engine thread: the book's top levels,
recent trades, candles, the account, our orders with their queue position, and metrics. Published as the latest value;
older ones are dropped, not queued.

**Report ring.** The fixed-size, numbered record of lifecycle steps, execution reports and control acknowledgements
that clients read and resume from.

## Evidence

**State digest** (or **digest**). A 64-bit hash of a state, computed in a fixed byte order so that it does not depend
on the host: of a book, of all books of a replay, of the execution state, or of the portfolio with every fill in order.
Equal digests show two runs reached the same state; they are the regression oracle.

**Determinism.** The property that the same input, configuration, build and commands give the same results and digests
on every run.

**Throughput.** Market events applied per wall second over a whole run.

**Latency** (of the engine). The cost of processing one event. Chronos reports the mean and the distribution of
1,024-event batch means, not single-event times. Not to be confused with the **order and report latencies**, which are
modelled delays in exchange time.

**Headless execution.** Running the engine without the server or UI: `simulator replay`, `simulator backtest` and the
benchmarks, none of which links the server.

**Invariant** (of the book). A structural property the book must keep, such as levels in strict price order or a
level's total equal to its orders' quantities, checked on request. Different from the architecture's
**invariants I1 to I14** ([README.md](README.md#the-invariants)), which are rules for changes.
