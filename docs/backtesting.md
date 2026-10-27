# Backtesting: the loop, the fill model and the strategies

A backtest replays one symbol's day through `EventLoop<Strategy>`. The strategy sees the historical L3 book after
each event and sends orders; `ExecutionSimulator` fills them as a queue-position-aware exchange would, without ever
touching the historical book.

```bash
./build/simulator backtest data/itch/12302019.NASDAQ_ITCH50.bin --symbol SPY --date 2019-12-30 --strategy mm
./build/simulator backtest --store data/itch/store --symbol AAPL --date 2019-12-30 --strategy orb
```

## D1: history and simulation never mix

- `MarketBook` is the historical L3 book of one symbol. `MarketBook::apply` is its only mutator, and only the replay
  loop and the event loop call it, with market events.
- The execution simulator, the portfolio and the strategies get `const` references to the book. Our orders live in
  `ExecutionSimulator` as shadow orders and are never inserted into `MarketBook`. Historical orders are never matched
  against each other.
- So a backtest leaves the same history digest as a plain replay of the same file, which `BacktestDeterminismTest`
  asserts, on synthetic data always and on a real day when `CHRONOS_ITCH_FILE` is set.

## D3: one deterministic loop

- `EventLoop<S>` merges the market stream with `SimQueue`, which holds only our events (`OrderArrive`,
  `CancelArrive`, `ReportDeliver`, `Timer`). Events run in (timestamp, stream, sequence) order, and a tie goes to the
  market event. That is pessimistic on purpose: an order that arrives at `t` queues behind every historical Add
  stamped `t`.
- Per market event, in this order:
  1. `ExecutionSimulator::on_market` on the pre-event book, because the fill rules need the historical order that the
     event references;
  2. `market_trade()`, the trade the event prints, read while the executed order is still in the book;
  3. `MarketBook::apply`;
  4. `Portfolio::mark` while a position is open;
  5. `Strategy::on_market`, only if the book applied the event, on the post-event book, with
     `StrategyContext::trade` pointing at that trade (null when the event printed none).
- Two constant latencies, 50,000 ns each by default: the order latency (strategy to exchange, which also stands for the
  market-data delay) and the report latency (exchange back to the strategy and the portfolio). An order sent in
  `on_market` arrives after the event that triggered it, so there is no look-ahead.
- At the end of the market stream, the events still in `SimQueue` are drained against the final book, at most 100,000
  of them. New timers are refused from then on.
- The loop never reads a wall clock: the same input and flags give the same output, byte for byte.

## D2: the fill model

From the comment in [`execution_simulator.hpp`](../include/simulator/sim/execution_simulator.hpp):

- **IOC**, at arrival: walk the visible opposite levels best first while they cross, and fill min(remaining, level
  quantity) at each level's price as taker. Whatever is left expires. Fills take no liquidity from the book (no market
  impact).
- **Day orders are post-only.** One that would take liquidity is rejected (`WouldCross`). Otherwise it rests
  virtually: `anchor_sequence` = the book's last sequence and `ahead_quantity` = the visible quantity at its price, so
  every historical order already at that price is ahead of it.
- **On each market event**, before the book applies it, for each of our resting orders `o`:
  - **(b)** an Execute, Cancel, Delete or Replace of a historical order ahead of `o` at `o`'s price shrinks
    `ahead_quantity`;
  - **(a)** an Execute of a historical order behind `o` at `o`'s price fills `o` at its price;
  - **(d)** an Execute of a same-side order at a worse price fills `o`: a trade-through would have hit `o` first;
  - **(c)** an Add, or a Replace's new order, that crosses `o` from the opposite side fills `o` at its price.
- **Trade-price guard.** Rules (a) and (d) fill `o` only if the trade price is not better than `o`'s price. The trade
  price is the execution price of an ITCH `C`, else the historical order's price.
- One event fills our orders in our priority order (best price, then arrival), and the total is capped at the event's
  quantity. Every such fill is maker liquidity.
- **Diagnostics.** A rule (a) or (d) fill while `ahead_quantity > 0` counts as a FIFO anomaly, and the first 10 are
  kept with their context. `backtest` prints maker fills and anomalies by rule, message type and session phase.

**The FIFO-anomaly counter found a fill-model bug.** A full-day SPY market-maker run before the trade-price guard had
583 anomalies among 13,539 maker fills. The fill attribution table traced every one to ITCH `C` executions of
price-slid orders: displayed at our price, but ranked and executed one tick better, so the aggressor never reached our
price. With the guard, the count is 0.

## Portfolio, risk and fees

- Reports are booked at delivery, a report latency after they happened, just before the strategy sees them.
- Longs are marked at the best bid and shorts at the best ask.
- Money is integer: tick-shares ($0.0001) for ITCH, with `__int128` products. PnL is realised + unrealised − fees.
- Fees are per share in price ticks, maker and taker apart; a negative fee is a rebate (`--maker-fee-ticks -20` is a
  $0.0020 rebate).
- `Portfolio::reserve` checks each order before it is sent, against `RiskLimits`: no larger than the largest order
  (by default the strategy's clip or quote size), at most `--max-open-orders` live, and a worst-case position (every
  open order on its side filled, this one included) within `--max-position`.

## The strategies

All three are toys, written to exercise the fill model and the loop, not to make money. Each implements the `Strategy`
concept in [`engine/strategy.hpp`](../include/simulator/engine/strategy.hpp): `on_market`, `on_report` and
`on_timer`, with a `StrategyContext` holding the const book, the const portfolio, the order gateway and the event's
trade.

| Strategy | `--strategy` | Does |
|---|---|---|
| `ImbalanceTaker` | `imbalance` (default) | an IOC of `--clip` shares when one top-of-book size is at least `--threshold` times the other, then waits `--cooldown-ms` |
| `MarketMaker` | `mm` | post-only quotes of `--quote-size` shares at the best bid and ask, each living at least `--requote-ms` before it moves |
| `Orb` | `orb` | the opening range breakout below |

### Opening range breakout (`Orb`)

- **The range** is the high and low of every execution's price in the half-open interval [09:30, 09:30 +
  `--range-minutes`), from `StrategyContext::trade`, so executions of hidden size count too.
- **Entries.** On the first trade strictly above the high, an IOC buy at the ask for `--clip` shares; strictly below the
  low, an IOC sell at the bid. One entry per direction a day; a direction is spent once the gateway accepts its entry.
  No reversal: a trade that stops a long does not also enter short.
- **Stop and flatten.** A trade strictly through the other side of the range closes the position; a timer at 15:55
  closes whatever is left. Closing orders are IOCs of at most `--clip` shares, sent again on each market event until
  the position is flat. A day starts by closing any position carried into it.
- **One order in flight**, and only while the book is trading and has a touch on the side it needs.
- **Stats**: entries, stops, flattens, closing orders, risk rejects, days that never formed a range, and the shares
  requested, bought, sold and expired.

On AAPL's full day, read from the store in under a second:

```text
Strategy
  entries          1 long, 1 short
  closing          1 stops, 1 flattens, 3 orders
  risk rejects     0
  no-range days    0
  last range       286.4400 to 289.7200
  shares           500 requested, 200 bought, 200 sold, 100 expired
```

It went long on the breakout, was stopped out, went short on a later trade, and was flattened at 15:55 after one
closing IOC expired unfilled and was sent again: flat at the end, a total PnL of −$146.00.

## Results on real data

| Run | Maker fills | Fill rate (of quoted shares) | FIFO anomalies | Total PnL |
|---|---|---|---|---|
| SPY market maker, full day, no fees | 12,843 | 19.69 % | 0 | −$2,726.78 |
| SPY market maker, 256 MiB slice (pre-market), no fees | 143 (13,693 shares) | 3.65 % | 0 | $4.77 |

Part of the slice run's output:

```text
Execution
  maker fills      143 (13693 shares)
  taker fills      0 (0 shares)
  fill rate        3.65% of 375000 shares quoted
  cancels          3612 done, 64 rejected (too late)
  would cross      1 rejected
  FIFO anomalies   0

Fill attribution (maker fills/FIFO anomalies; crosses = first second of 09:30, 16:00)
  rule, msg         pre-open    open cross       regular   close cross    post-close
  (a) E                 26/0           2/0           3/0           0/0           0/0
  (a) C                  0/0           0/0           0/0           0/0           0/0
  (d) E                  5/0           0/0           1/0           0/0           0/0
  (d) C                  0/0           0/0           0/0           0/0           0/0
  (c) Add               86/0           2/0          14/0           0/0           0/0
  (c) Replace            0/0           0/0           4/0           0/0           0/0

Portfolio
  position         -233 shares
  cash             $75289.4000
  realised PnL     $10.7069
  unrealised PnL   -$5.9369
  fees             $0.0000
  total PnL        $4.7700 (realised + unrealised - fees)
  max drawdown     $141.0200
  max |position|   468 shares
  open orders      2 at the end
```

Its history digest, `e9f436a5ac864f40`, equals that of `replay --symbol SPY` on the same file: the strategy's orders
did not change the replayed book. The full-day row was recorded on 2026-10-13 at commit `a5ed53b`; re-run the command
at the top to reproduce it.

## What the model leaves out

See [limitations.md](limitations.md): no market impact, IOC and post-only orders only, constant latencies, no
regulatory sell fees yet, and hidden liquidity that never fills our orders.
