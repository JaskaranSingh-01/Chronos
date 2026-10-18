import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { Candle, Execution, Hello, ServerMessage, SessionState, Snapshot } from './protocol';
import { apply, createStore, initial, mergeBars, selectCanTrade, selectOrders, withHistory, type Env } from './store';

const T = 1_577_716_200_000_000_000; // 2019-12-30 09:30:00 ET, epoch ns
const S = 1_000_000_000;

const hello = (over: Partial<Hello> = {}): Hello => ({
  type: 'hello',
  session_id: 's1',
  generation: 2,
  protocol: 1,
  role: 'controller',
  price_scale: 4,
  quantity_scale: 0,
  fees: { maker: 0, taker: 0 },
  limits: { max_position: 10_000, max_order_quantity: 1_000, max_open_orders: 16 },
  ...over,
});

const snapshot = (over: Partial<Snapshot> = {}): Snapshot => ({
  type: 'snapshot',
  session_id: 's1',
  generation: 2,
  symbol: 'ACME',
  snapshot_seq: 1,
  exchange_ts: T,
  event_seq: 3,
  last_report_seq: 0,
  last_command_seq: 0,
  state: 'paused',
  speed: 1,
  progress: 0.5,
  failure: 'none',
  source_error: 'none',
  trading_state: 'trading',
  bids: [[100_000, 300, 1]],
  asks: [[100_100, 200, 1]],
  trades: [],
  trades_truncated: false,
  candle: null,
  closed_candles: [],
  candles_truncated: false,
  account: { position: 0, cash: 0, realised: 0, unrealised: 0, fees: 0, pnl: 0, max_drawdown: 0 },
  my_orders: [],
  my_orders_truncated: false,
  metrics: { events_per_second: 0, dropped_snapshots: 0, command_queue_depth: 0, log_failed: false },
  ...over,
});

const fill = (seq: number, generation = 2): Execution => ({
  type: 'report',
  kind: 'execution',
  session_id: 's1',
  generation,
  seq,
  order_id: 1,
  report_type: 'fill',
  side: 'buy',
  price: 100_100,
  quantity: 100,
  leaves: 0,
  reject_reason: 'none',
  liquidity: 'taker',
  exchange_ts: T,
});

const bar = (second: number, close: number, volume = 100): Candle => [
  T + second * S,
  close,
  close,
  close,
  close,
  volume,
];

const generationStart = (generation: number, seq: number): ServerMessage => ({
  type: 'report',
  kind: 'generation_start',
  session_id: 's1',
  generation,
  seq,
  symbol: 'ACME',
  strategy: 'none',
});

const controlAck = (seq: number, control: string, state: SessionState): ServerMessage => ({
  type: 'ack',
  session_id: 's1',
  generation: 2,
  seq,
  control,
  control_seq: seq,
  state,
});

const run = (...messages: ServerMessage[]) => messages.reduce(apply, initial);

describe('apply', () => {
  it('starts a fresh client at the state watermark, and a reconnect keeps its cursor for the resync', () => {
    const fresh = run(hello(), snapshot({ type: 'state', last_report_seq: 5 }));
    expect(fresh.seq).toBe(5);
    const reconnected = run(
      hello(),
      snapshot({ type: 'state', last_report_seq: 5 }),
      hello(),
      snapshot({ type: 'state', snapshot_seq: 2, last_report_seq: 9 }),
    );
    expect(reconnected.seq).toBe(5);
    expect(apply(reconnected, fill(6)).fills).toHaveLength(1);
  });

  it('adds no trade twice when a reconnect state repeats the last snapshot', () => {
    const first = snapshot({ type: 'state', trades: [[T, 100_100, 100, 'sell']], candle: bar(0, 100_100) });
    const s = run(hello(), first, hello(), { ...first });
    expect(s.tape).toEqual([[T, 100_100, 100, 'sell']]);
    expect(s.bars).toEqual([bar(0, 100_100)]);
  });

  it('dedupes reports by seq and keeps only this generation fills', () => {
    const s = run(hello(), snapshot({ type: 'state', last_report_seq: 5 }), fill(6), fill(6), fill(5), fill(7, 1));
    expect(s.fills.map((f) => f.seq)).toEqual([6]);
    expect(s.seq).toBe(7);
  });

  it('resets the book, chart, tape and fills on a newer generation in any envelope, and ignores older snapshots', () => {
    const before = run(
      hello(),
      snapshot({ type: 'state', trades: [[T, 100_100, 100, 'sell']], candle: bar(0, 100_100) }),
      fill(1),
    );
    expect([before.tape.length, before.bars.length, before.fills.length]).toEqual([1, 1, 1]);
    const after = apply(before, generationStart(3, 2));
    expect(after.generation).toBe(3);
    expect([after.snap, after.tape, after.bars, after.fills]).toEqual([null, [], [], []]);
    expect(after.barsVersion).toBeGreaterThan(before.barsVersion);
    expect(apply(after, snapshot({ snapshot_seq: 9 })).snap).toBeNull();
    expect(apply(before, hello({ generation: 3 })).bars).toEqual([]); // a reconnect's hello is an envelope too
    expect(apply(after, snapshot({ type: 'state', generation: 3, snapshot_seq: 9 })).snap?.generation).toBe(3);
  });

  it('drops everything when hello names a new session', () => {
    const old = run(hello(), snapshot({ type: 'state', last_report_seq: 40, candle: bar(0, 100_100) }), fill(41));
    const s = apply(old, hello({ session_id: 's2', generation: 1 }));
    expect([s.seq, s.generation, s.fills, s.bars, s.snap]).toEqual([0, 1, [], [], null]);
    expect(apply(s, fill(41)).fills).toEqual([]); // still s1's
  });

  it('marks the history incomplete after a gap and starts again at the full state watermark', () => {
    const s = run(
      hello(),
      snapshot({ type: 'state', last_report_seq: 5 }),
      { type: 'gap', session_id: 's1', generation: 2, from: 6, to: 70_000 },
      snapshot({ type: 'state', full_resync: true, snapshot_seq: 2, last_report_seq: 70_100 }),
    );
    expect(s.historyIncomplete).toBe(true);
    expect(s.seq).toBe(70_100);
  });

  it('tells the two ack shapes apart: a control ack moves the cursor and the state, a command ack neither', () => {
    const s = run(hello(), snapshot({ type: 'state', last_report_seq: 5 }));
    const control = apply(s, controlAck(6, 'play', 'playing'));
    expect([control.seq, control.state]).toEqual([6, 'playing']);
    const command = apply(control, {
      type: 'ack',
      session_id: 's1',
      generation: 2,
      command_seq: 3,
      persisted: 'flushed',
    });
    expect(command).toBe(control);
  });

  it('dedupes control acks by seq', () => {
    const s = run(hello(), snapshot({ type: 'state', last_report_seq: 5 }), controlAck(6, 'play', 'playing'));
    expect(apply(s, controlAck(6, 'pause', 'paused'))).toBe(s);
    expect(apply(s, controlAck(5, 'pause', 'paused'))).toBe(s);
  });

  it('keeps the newer state a reconnect brought when older control acks are replayed after it', () => {
    const reconnect = (state: SessionState, watermark: number) =>
      run(
        hello(),
        snapshot({ type: 'state', state: 'playing', last_report_seq: 5 }),
        hello(),
        snapshot({ type: 'state', snapshot_seq: 2, state, last_report_seq: watermark }),
      );
    const ended = apply(reconnect('ended', 6), controlAck(6, 'play', 'playing'));
    expect([ended.state, ended.seq]).toEqual(['ended', 6]);
    const paused = reconnect('paused', 8);
    const seek = apply(paused, controlAck(7, 'seek', 'seeking'));
    expect([seek.state, seek.seq]).toEqual(['paused', 7]);
    expect(apply(paused, controlAck(9, 'play', 'playing')).state).toBe('playing'); // past the watermark: live
  });

  it('records the position of a running seek', () => {
    const s = run(hello(), snapshot({ type: 'state' }), {
      type: 'progress',
      session_id: 's1',
      generation: 2,
      seek: T + 5 * S,
    });
    expect(s.seekAt).toBe(T + 5 * S);
  });

  it('shows errors and rejections, naming an order by its client_id', () => {
    const s = run(hello(), snapshot({ type: 'state' }));
    const error = apply(s, {
      type: 'error',
      session_id: 's1',
      generation: 2,
      code: 'not_ready',
      message: 'command refused',
      command_seq: 4,
      client_id: 7,
    });
    expect(error.notice).toBe('order 7: not_ready: command refused');
    const rejected = apply(s, {
      type: 'report',
      kind: 'lifecycle',
      session_id: 's1',
      generation: 2,
      seq: 1,
      step: 'simulator_rejected',
      reason: 'would_cross',
      command: 'order',
      command_seq: 4,
      client_id: 8,
      order_id: 2,
      exchange_ts: T,
      market_event_seq: 3,
    });
    expect(rejected.notice).toBe('order 8 simulator_rejected: would_cross');
  });

  it('merges bars by start, and history wins up to its last bar and trade', () => {
    expect(mergeBars([bar(0, 1), bar(1, 1)], [bar(1, 2), bar(2, 3)])).toEqual([bar(0, 1), bar(1, 2), bar(2, 3)]);
    const live = run(
      hello(),
      snapshot({
        type: 'state',
        candle: bar(1, 5),
        trades: [
          [T + S, 100_000, 10, 'buy'],
          [T + 2 * S, 100_000, 20, 'buy'],
        ],
      }),
    );
    const s = withHistory(live, {
      session_id: 's1',
      generation: 2,
      candles: [bar(0, 1), bar(1, 2)],
      trades: [[T + S, 100_000, 10, 'buy']],
    });
    expect(s.bars).toEqual([bar(0, 1), bar(1, 2)]);
    expect(s.tape.map((t) => t[2])).toEqual([10, 20]);
    const later = apply(
      s,
      snapshot({
        snapshot_seq: 2,
        trades: [
          [T + S, 100_000, 10, 'buy'],
          [T + 3 * S, 100_000, 30, 'buy'],
        ],
      }),
    );
    expect(later.tape.map((t) => t[2])).toEqual([10, 20, 30]);
  });

  it('records the spans the book was not trading', () => {
    const s = run(
      hello(),
      snapshot({ type: 'state', exchange_ts: T }),
      snapshot({ snapshot_seq: 2, exchange_ts: T + S, trading_state: 'halted' }),
      snapshot({ snapshot_seq: 3, exchange_ts: T + 5 * S, trading_state: 'halted' }),
      snapshot({ snapshot_seq: 4, exchange_ts: T + 6 * S }),
      snapshot({ snapshot_seq: 5, exchange_ts: T + 9 * S, trading_state: 'paused' }),
    );
    expect(s.halts).toEqual([
      [T + S, T + 5 * S],
      [T + 9 * S, T + 9 * S],
    ]);
  });
});

describe('selectors', () => {
  it('return the same references when their slice did not change, so panels skip the render', () => {
    expect(selectOrders(initial)).toBe(selectOrders({ ...initial, notice: 'x' }));
    const s = run(hello(), snapshot({ type: 'state' }));
    const next = apply(s, snapshot({ snapshot_seq: 2 })); // its own parsed, empty my_orders
    expect(next.snap).not.toBe(s.snap);
    expect(selectOrders(next)).toBe(selectOrders(s));
    expect(selectOrders(s)).toBe(selectOrders(initial));
    expect(apply(s, fill(1)).snap).toBe(s.snap);
  });

  it('allow trading only to a connected controller on a trading symbol that is neither seeking nor ended', () => {
    const s = { ...run(hello(), snapshot({ type: 'state' })), status: 'open' as const };
    expect(selectCanTrade(s)).toBe(true);
    expect(selectCanTrade({ ...s, hello: hello({ role: 'viewer' }) })).toBe(false);
    expect(selectCanTrade({ ...s, status: 'closed' })).toBe(false);
    expect(selectCanTrade({ ...s, state: 'seeking' })).toBe(false);
    expect(selectCanTrade({ ...s, snap: snapshot({ trading_state: 'halted' }) })).toBe(false);
    expect(selectCanTrade({ ...s, snap: snapshot({ symbol: '' }) })).toBe(false);
  });
});

describe('createStore', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  // The server's answer for any generation of any session: empty, and about the session and generation asked for.
  const echo = (path: string) => {
    const query = new URL(path, 'http://x').searchParams;
    return Response.json({
      session_id: query.get('session_id'),
      generation: Number(query.get('generation')),
      candles: [],
      trades: [],
    });
  };

  function fake({ statuses = [], history = echo }: { statuses?: number[]; history?: (path: string) => unknown } = {}) {
    const sent: string[] = [];
    const fetched: string[] = [];
    const saved = new Map<string, string>();
    let socket: Parameters<Env['connect']>[1] | undefined;
    let connects = 0;
    const env: Env = {
      async fetch(path, init) {
        const auth = (init?.headers as Record<string, string> | undefined)?.Authorization;
        fetched.push(auth ? `${path} ${auth}` : path);
        if (path.startsWith('/api/history')) return (await history(path)) as Response;
        const status = statuses.shift() ?? 200;
        return status === 200 ? Response.json({ symbols: ['ACME'] }) : new Response(null, { status });
      },
      connect(_url, on) {
        socket = on;
        connects++;
        return { send: (text) => void sent.push(text) };
      },
      load: (key) => saved.get(key) ?? null,
      save: (key, value) => void saved.set(key, value),
      host: () => '127.0.0.1:8080',
    };
    const histories = () => fetched.filter((path) => path.startsWith('/api/history')).length;
    return { env, sent, fetched, saved, socket: () => socket!, connects: () => connects, histories };
  }

  const json = (m: ServerMessage) => JSON.stringify(m);
  const settle = () => vi.advanceTimersByTimeAsync(0);

  // A started store with an open socket, fed `messages`.
  async function connected(f: ReturnType<typeof fake>, ...messages: ServerMessage[]) {
    const store = createStore(f.env);
    await store.start();
    f.socket().open();
    messages.forEach((m) => f.socket().message(json(m)));
    return store;
  }

  it('in loopback mode sends no auth, fetches history after a state, and resyncs from last+1 before hello on a reconnect', async () => {
    const f = fake();
    const store = createStore(f.env);
    await store.start();
    f.socket().open();
    f.socket().message(json(hello()));
    f.socket().message(json(snapshot({ type: 'state', last_report_seq: 5 })));
    await vi.waitFor(() => expect(f.fetched).toContain('/api/history?session_id=s1&generation=2&from_ts=0'));
    expect(f.sent).toEqual([]);
    expect(store.get().symbols).toEqual(['ACME']);

    f.socket().close();
    expect(store.get().status).toBe('closed');
    await vi.advanceTimersByTimeAsync(1_000);
    f.socket().open();
    expect(f.sent.map((t) => JSON.parse(t))).toEqual([
      { type: 'resync', session_id: 's1', generation: 2, from_report_seq: 6 },
    ]);
  });

  it('in token mode retries with the stored Bearer token and authenticates first; without one it asks', async () => {
    const f = fake({ statuses: [401, 200] });
    f.saved.set('chronos-token', 't0k3n');
    const store = createStore(f.env);
    await store.start();
    expect(f.fetched).toEqual(['/api/symbols', '/api/symbols Bearer t0k3n']);
    f.socket().open();
    expect(JSON.parse(f.sent[0])).toEqual({ type: 'auth', token: 't0k3n' });

    const none = fake({ statuses: [401] });
    const asking = createStore(none.env);
    await asking.start();
    expect(asking.get().status).toBe('token');
  });

  it('in token mode a reconnect authenticates before it resyncs', async () => {
    const f = fake({ statuses: [401, 200, 401, 200] });
    f.saved.set('chronos-token', 't0k3n');
    const store = await connected(f, hello(), snapshot({ type: 'state', last_report_seq: 5 }));
    f.sent.length = 0;
    f.socket().close();
    await vi.advanceTimersByTimeAsync(1_000);
    await vi.waitFor(() => expect(f.connects()).toBe(2));
    f.socket().open();
    expect(f.sent.map((t) => JSON.parse(t).type)).toEqual(['auth', 'resync']);
    expect(store.get().status).toBe('open');
  });

  it('asks the user to open the UI on loopback when the server answers 403', async () => {
    const f = fake({ statuses: [403] });
    const store = createStore(f.env);
    await store.start();
    expect(store.get().status).toBe('host');
    expect(f.connects()).toBe(0);
  });

  it('sends commands with the newest generation and a fresh client_id, and never queues one while disconnected', async () => {
    const f = fake();
    const store = await connected(f, hello(), generationStart(3, 1));
    expect(store.order({ side: 'buy', order_type: 'ioc', qty: 100, price: 100_100 })).toBe(true);
    expect(store.control({ action: 'step', n: 1 })).toBe(true);
    expect(f.sent.map((t) => JSON.parse(t))).toEqual([
      {
        type: 'order',
        client_id: 1,
        side: 'buy',
        order_type: 'ioc',
        qty: 100,
        price: 100_100,
        session_id: 's1',
        generation: 3,
      },
      { type: 'control', action: 'step', n: 1, session_id: 's1', generation: 3 },
    ]);
    f.socket().close();
    expect(store.cancel(1)).toBe(false);
    expect(store.get().notice).toBe('not connected');
    expect(f.sent).toHaveLength(2);
  });

  it('sends a market order with no price key', async () => {
    const f = fake();
    const store = await connected(f, hello());
    expect(store.order({ side: 'sell', order_type: 'market', qty: 50 })).toBe(true);
    const order = JSON.parse(f.sent[0]);
    expect(order).toEqual({
      type: 'order',
      client_id: 1,
      side: 'sell',
      order_type: 'market',
      qty: 50,
      session_id: 's1',
      generation: 2,
    });
    expect(order).not.toHaveProperty('price');
  });

  it('opens one socket however many times start runs, so none is orphaned holding the controller seat', async () => {
    const f = fake();
    const store = createStore(f.env);
    await Promise.all([store.start(), store.start()]);
    expect(f.connects()).toBe(1);
    await store.start(); // while connected
    expect(f.connects()).toBe(1);
  });

  it('follows a server restart: a new session_id resets everything and takes the new watermark', async () => {
    const f = fake();
    const store = await connected(
      f,
      hello(),
      snapshot({ type: 'state', last_report_seq: 40, candle: bar(0, 100_100) }),
      fill(41),
    );
    expect([store.get().seq, store.get().fills.length, store.get().bars.length]).toEqual([41, 1, 1]);
    f.socket().close();
    await vi.advanceTimersByTimeAsync(1_000);
    await vi.waitFor(() => expect(f.connects()).toBe(2));
    f.socket().open();
    f.socket().message(json(hello({ session_id: 's2', generation: 1 })));
    f.socket().message(json(snapshot({ type: 'state', session_id: 's2', generation: 1, last_report_seq: 3 })));
    const s = store.get();
    expect([s.hello?.session_id, s.generation, s.seq, s.fills, s.bars]).toEqual(['s2', 1, 3, [], []]);
    f.sent.length = 0;
    expect(store.control({ action: 'pause' })).toBe(true);
    expect(JSON.parse(f.sent[0])).toEqual({ type: 'control', action: 'pause', session_id: 's2', generation: 1 });
  });

  it('refetches history after a state, a truncated snapshot or a skipped snapshot_seq, and not after consecutive ones', async () => {
    const f = fake();
    const store = await connected(f, hello());
    const feed = (over: Partial<Snapshot>) => f.socket().message(json(snapshot(over)));
    const refetched = async (n: number) => {
      await vi.waitFor(() => expect(f.histories()).toBe(n));
      await settle();
    };

    feed({ type: 'state' });
    await refetched(1);
    feed({ snapshot_seq: 2 });
    feed({ snapshot_seq: 3 });
    await settle();
    expect(f.histories()).toBe(1);

    feed({ snapshot_seq: 5 }); // 4 was published and missed
    await refetched(2);
    feed({ snapshot_seq: 6 });
    await settle();
    expect(f.histories()).toBe(2);

    feed({ snapshot_seq: 7, trades_truncated: true });
    await refetched(3);
    feed({ snapshot_seq: 8, candles_truncated: true });
    await refetched(4);
    expect(store.get().snap?.snapshot_seq).toBe(8);
  });

  it('keeps one history fetch in flight and one pending: a state and ten truncated snapshots fetch twice', async () => {
    let release!: () => void;
    const gate = new Promise<void>((resolve) => (release = resolve));
    const f = fake({
      history: async (path) => {
        await gate;
        return echo(path);
      },
    });
    await connected(f, hello(), snapshot({ type: 'state' }));
    for (let seq = 2; seq <= 11; seq++)
      f.socket().message(json(snapshot({ snapshot_seq: seq, trades_truncated: true })));
    expect(f.histories()).toBe(1);
    release();
    await vi.waitFor(() => expect(f.histories()).toBe(2));
    await settle();
    expect(f.histories()).toBe(2);
  });

  it('applies a current history, discards a stale or conflicting one, and retries after a failed fetch', async () => {
    const answers: (() => unknown)[] = [
      () => Response.json({ session_id: 's1', generation: 2, candles: [bar(0, 1)], trades: [] }),
      () => Response.json({ session_id: 's1', generation: 1, candles: [bar(5, 9)], trades: [] }), // an old generation
      () => Response.json({ session_id: 's0', generation: 2, candles: [bar(6, 9)], trades: [] }), // another session
      () => new Response(null, { status: 409 }),
      () => {
        throw new Error('offline');
      },
      () => Response.json({ session_id: 's1', generation: 2, candles: [bar(0, 1), bar(1, 2)], trades: [] }),
    ];
    const f = fake({ history: () => answers[f.histories() - 1]() });
    const store = await connected(f, hello());
    const feed = (seq: number) =>
      f.socket().message(json(snapshot({ type: seq === 1 ? 'state' : 'snapshot', snapshot_seq: seq })));

    feed(1);
    await vi.waitFor(() => expect(store.get().bars).toEqual([bar(0, 1)]));
    for (const [n, seq] of [
      [2, 3],
      [3, 5],
      [4, 7],
      [5, 9],
    ]) {
      feed(seq); // each skips one, so each asks for history
      await vi.waitFor(() => expect(f.histories()).toBe(n));
      await settle();
      expect(store.get().bars).toEqual([bar(0, 1)]);
    }
    feed(11);
    await vi.waitFor(() => expect(store.get().bars).toEqual([bar(0, 1), bar(1, 2)]));
    expect(f.histories()).toBe(6);
  });
});
