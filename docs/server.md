# chronos-serve: sessions, the Strategy Lab and the protocol

`chronos-serve` serves the Trading Terminal and the Strategy Lab ([ui.md](ui.md)) from one process:

```bash
./build/chronos-serve --data data/itch --date 2019-12-30     # then open http://127.0.0.1:8080/
```

`--data` names a directory of days, each named `MMDDYYYY.NASDAQ_ITCH50.bin`; `--date` picks the starting day, the latest
by default. Each new day is read once into the per-symbol store ([market-data.md](market-data.md#the-per-symbol-store)),
about a minute a day, before it is served. Every option is in [cli.md](cli.md#chronos-serve).

## A session

The server runs one interactive session: a `ReplaySession` on a thread of its own (`SessionRunner`), the only thread in
the process that paces a replay by a clock.

- **Generations.** Loading a symbol (and its days) starts a new generation: a fresh `EventLoop` over that symbol's
  segments, paused before its first event. Every message carries the session id and the generation, so a client never
  confuses an old generation's data with the new one's; a command stamped with an old generation is refused
  (`stale_generation`).
- **Controls.** Play, pause, step N events, speed (1×, 10×, 100× or Max), seek to an exchange time (the replay runs
  forward to it at full speed, sending `progress`), cancel a seek, restart, load a symbol, and watch a Lab run.
- **Orders.** Post-only, IOC and market orders go through the backtests' `ExecutionSimulator`: 50 µs to arrive, 50 µs
  for each report, a place in the L3 queue. A market order is an IOC limited to 1 % beyond the best opposite price,
  rounded to the cent. A price off the cent grid at $1 or more is rejected (`off_tick`), as Reg NMS Rule 612 requires.
  Limits: 10,000 shares of position, 1,000 shares an order, 16 open orders.
- **A strategy in the session.** `attach` runs a registered strategy against the live replay from the next event, and
  `detach` stops it; each cancels whatever rests first. Manual orders are refused while one is attached.
- **One controller.** The first tab to connect controls the session; any other is a viewer that sees everything and
  sends nothing, and is told why.

### The command log

Every command the session applies is appended to `sessions/<id>.jsonl` in the working directory, after a header that
records the build, the symbol, each day's hash and segment hash, and the strategy attached from the start, by name with
every param. A generation's log replays it exactly:
`CommandLogTest` reads a log back and reproduces the session's digests. `GET /api/sessions/<id>/log` serves it.

A command is admitted once its lines are queued for the log's writer, and the engine may apply it from then on. The
client's `ack` waits until the command's admitted line is written and flushed to the operating system; `--fsync` also
syncs the file to the device at each command's lifecycle line. The writer appends lines in order; while its queue is
more than half full, the engine holds market work for it. If a write fails (a full disk, say), or the queue fills, the
log is marked failed. A failed write loses the lines still queued; a full queue, the line that did not fit and every
one after it. A command whose admitted line was never written is answered `log_unavailable` (the engine may already
have applied it), and every later command is refused, so the log replays the session up to the failure.

### Backpressure

- **Snapshots** (book, trades, the current 1 s candle, our orders and account, metrics) go out at up to 30 a second.
  The engine publishes each through a latest-value slot and never waits for a reader: `dropped_snapshots` counts the
  ones it published while the sender held the slot. Every connection is sent the newest at each tick; a client that
  stops reading leaves its frames buffered in the server, about 1 MB a second (see
  [limitations.md](limitations.md#what-the-server-assumes)).
- **Reports** (executions, lifecycle steps, generation marks, control acks) go through a ring of the last 65,536. A
  client that reconnects resyncs from its last seq; one that fell past the ring gets a `gap` and refetches history.
- **History** (`GET /api/history`) gives a generation's 1 s candles and trades from a time on, so a reconnecting page
  redraws its chart without replaying anything.

## The Strategy Lab

The Lab runs backtests on a worker thread of its own, one at a time with up to 8 waiting, while the terminal plays: a
registered strategy over one symbol and one or more days, at full speed.

- **A run** is posted as `{"symbol":"SPY","days":["2019-12-30"],"strategy":"market_maker"}`. Every other key takes
  `simulator backtest`'s default: `params`, `latency` (`order_ns`, `report_ns`), `fees` (`maker`, `taker`, in ticks a
  share), `limits` and `window`. The window is `"regular"` by default, so a strategy trades only inside each day's feed
  session, from its own 'Q' to its 'M' System Event (09:30-16:00 when a file lacks them); `"full_day"` trades the whole
  file. A one-day, full-day run with the defaults gives `simulator backtest`'s PnL, fills and fill attribution.
- **Strategies**: every plugin in `strategies/` ([strategies.md](strategies.md)), by name: `imbalance`,
  `market_maker`, `orb` and `trend_taker` ship. `GET /api/strategies` lists each with its label, summary, params
  (default, min, max, help) and default limits. The market maker's `improve_ticks` counts ticks of the price grid, a
  cent each at $1 or more. An unknown param, an integer param given a fraction, or a value past its cap is refused with
  the field named; a run its strategy could never trade, such as an ORB clip above the position or order-size limit, is
  refused naming the param.
- **Saved runs.** Each finished run is written to `runs/<id>.json` with the build it ran on (commit, source digest,
  compiler), each day's hash and its symbol's segment (`segment: {hash, size}`), the regular hours, and its results: the
  PnL at the end of every second with a market event, PnL by day and by hour, the first 50,000 fills, the fill
  attribution and the strategy's own stats. A restart reloads them.
- **Verify** re-runs a saved run, reading its segments afresh and checking them as read, and records a verdict:
  `reproduced` (the same digests: the book's; the execution counters and resting orders; and the portfolio's, which
  hashes every fill in order, then the totals), `mismatch`, `different_code` (this build's source digest is not the
  run's, so nothing is proven either way) or `input_changed` (a segment is not the one the run read).
- **Watch in Terminal** replays a saved run in the terminal, paced, at any speed. When its replay ends the server sends
  `report{kind:"reproduced"}` with the verdict, by Verify's rules: a command applied during the Watch that changes a
  digest, an order or a strategy's fills, gives `mismatch`. The Watch's own command log names the run and reproduces
  it.

## HTTP API

| Endpoint | Does |
|---|---|
| `GET /api/days` | the days in `--data` whose store is built: date, file, hash, size, session hours, symbol count |
| `GET /api/symbols[?days=D1,D2]` | the symbols with a market event on every day given (default the starting day) |
| `GET /api/history?session_id=&generation=&from_ts=` | a generation's 1 s candles and trades from `from_ts` on |
| `GET /api/sessions/<id>/log` | a session's command log |
| `GET /api/strategies` | the strategies, their params, defaults and caps, the windows, `max_days` and `max_queued` |
| `POST /api/backtests` | queues a run; 202 `{"id"}`, 400 `{"error","field"}` or `symbol_missing` with its `days`, 429 when 8 wait |
| `GET /api/backtests` | every run, without its results: state, progress, PnL and fills |
| `GET /api/backtests/<id>` | the whole run: spec, provenance, results |
| `POST /api/backtests/<id>/cancel` | cancels a queued or running run; 409 once it has ended |
| `DELETE /api/backtests/<id>` | removes a run and its file; a running one stops and is never saved |
| `POST /api/backtests/<id>/verify` | re-runs a completed run and records the verdict; 409 if partial, if a day's segment changed, or if recorded before the per-symbol store |
| `GET /ws` | the WebSocket |

## The WebSocket protocol

JSON text frames. Prices are integer ticks, quantities shares, times epoch nanoseconds. Levels, trades and candles are
arrays, to keep snapshots small: level `[price, quantity, orders]`, trade `[ts, price, quantity, resting side]`, candle
`[start, open, high, low, close, volume]`. Every server message carries `session_id` and `generation`.

**Server to client:**

| `type` | Carries |
|---|---|
| `hello` | the protocol version (2), the session id, the generation, this tab's role (`controller` or `viewer`), the scales |
| `state` | a full snapshot, sent on connect and on a new generation: book, trades, candles, our orders and account, the generation's spec and days, the run it watches, metrics |
| `snapshot` | the same, up to 30 a second while the replay moves |
| `report` | `kind`: `execution` (accepted, rejected, fill with its rule and queue ahead at entry, cancelled, cancel_rejected, expired), `lifecycle` (a command's steps), `generation_start`, `generation_end`, `reproduced` (a Watch's verdict); each with a `seq` |
| `ack` | a control's or command's acceptance, with its `seq` |
| `gap` | reports `from`-`to` fell out of the ring before this client read them |
| `progress` | a running seek's exchange time |
| `error` | a refusal: `code`, `message`, and the client id or command seq it answers |

**Client to server:**

| `type` | Carries |
|---|---|
| `auth` | the token, first, when the server has one |
| `resync` | the session, generation and first report seq this client still needs |
| `control` | `action`: `play`, `pause`, `restart`, `cancel_seek`, `step` (`n`), `speed` (`speed`, 0 = Max), `seek` (`ts`), `symbol` (`symbol`, `days`), `watch` (`run_id`) |
| `order` | `client_id`, `side`, `order_type` (`post_only`, `ioc`, `market`), `price` (not for `market`), `qty` |
| `cancel` | `order_id` |
| `attach` | `strategy` (a registered name) and its `params`: runs it in the session |
| `detach` | stops the attached strategy |

Every client message is checked against its schema: exactly its keys, each of its type and in range, at most 64 KiB.

## Security

A local tool, still defensive:

- It binds `127.0.0.1` by default. Any other `--host` needs a token (`--token`, or one is made and printed once):
  `/api` wants `Authorization: Bearer <token>`, and a WebSocket's first message is `auth`.
- Without a token, `/api` and the WebSocket answer only requests whose `Host` is `127.0.0.1` or `localhost`, so a
  DNS-rebinding page cannot reach them. A WebSocket is accepted only from the page this server served: its `Origin`
  must be `http://<Host>`, or with a token also `https://<Host>`, for a TLS reverse proxy in front.
- A POST or DELETE must be `Content-Type: application/json` (else 415) and carry the `Origin` of the page this server
  served (else 403), so no other site can queue or delete a run. A body over 64 KiB closes the connection.
- Session and run ids are the UTC time and 64 random bits, never derived from input.
- The token guards trading, not availability: behind anything public, put a buffering reverse proxy in front.

## Running it in Docker

`docker compose up` builds an image with `chronos-serve`, the CLI and the built UI, and serves the days in `data/itch`
(read-only), or two generated sample days when there are none. See [getting-started.md](getting-started.md#docker).
