import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type {
  Candle,
  Day,
  Execution,
  Hello,
  MyOrder,
  ServerMessage,
  SessionState,
  Snapshot,
  Spec,
  Trade,
} from './protocol';
import {
  apply,
  createStore,
  initial,
  mergeBars,
  selectOrders,
  selectTradeBlock,
  withHistory,
  type Env,
} from './store';

const T = 1_577_716_200_000_000_000; // 2019-12-30 09:30:00 ET, epoch ns
const S = 1_000_000_000;

const hello = (over: Partial<Hello> = {}): Hello => ({
  type: 'hello',
  session_id: 's1',
  generation: 2,
  protocol: 2,
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

const generationStart = (generation: number, seq: number, symbol = 'ACME'): ServerMessage => ({
  type: 'report',
  kind: 'generation_start',
  session_id: 's1',
  generation,
  seq,
  symbol,
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

  it('is seeking after a reconnect mid-seek: the stale pre-seek state is corrected by the next progress', () => {
    const progress: ServerMessage = { type: 'progress', session_id: 's1', generation: 2, seek: T + 5 * S };
    const seeking = run(hello(), snapshot({ type: 'state', last_report_seq: 5 }), controlAck(6, 'seek', 'seeking'));
    expect(seeking.state).toBe('seeking');
    const reconnected = run(
      hello(),
      snapshot({ type: 'state', last_report_seq: 5 }),
      controlAck(6, 'seek', 'seeking'),
      hello(),
      snapshot({ type: 'state', snapshot_seq: 2, state: 'paused', last_report_seq: 5 }), // none since the seek began
    );
    expect(reconnected.state).toBe('paused');
    const s = apply(reconnected, progress);
    expect([s.state, s.seekAt]).toEqual(['seeking', T + 5 * S]);
    expect(selectTradeBlock({ ...s, status: 'open' })).toBe('Seeking');
  });

  it('knows the symbol from generation_start alone, as after a seek that made a generation and no snapshot', () => {
    const s = run(hello(), snapshot({ type: 'state' }));
    expect(s.symbol).toBe('ACME');
    const seek = apply(s, generationStart(3, 2, 'MSFT'));
    expect([seek.symbol, seek.snap]).toEqual(['MSFT', null]);
    expect(apply(seek, generationStart(2, 3, 'ACME')).symbol).toBe('MSFT'); // an older generation's start
    expect(apply(seek, snapshot({ generation: 3, symbol: 'MSFT' })).symbol).toBe('MSFT');
    expect(apply(s, hello({ generation: 3 })).symbol).toBe(''); // a new generation's symbol is not yet known
    expect(run(hello({ generation: 1 }), snapshot({ generation: 1, symbol: '' })).symbol).toBe(''); // the empty ticker
  });

  it("takes the generation's days from its state and its generation_start, and forgets them for a new one", () => {
    const spec: Spec = {
      symbol: 'ACME',
      days: ['2019-12-27', '2019-12-30'],
      strategy: 'none',
      params: {},
      latency: { order_ns: 50_000, report_ns: 50_000 },
      fees: { maker: 0, taker: 0 },
      limits: { max_position: 10_000, max_order_quantity: 1_000, max_open_orders: 16 },
      window: 'full_day',
    };
    const s = run(hello(), snapshot({ type: 'state', spec }));
    expect(s.genDays).toEqual(['2019-12-27', '2019-12-30']);
    expect(apply(s, snapshot({ snapshot_seq: 2 })).genDays).toBe(s.genDays); // a snapshot carries none
    const start: ServerMessage = {
      type: 'report',
      kind: 'generation_start',
      session_id: 's1',
      generation: 3,
      seq: 1,
      symbol: 'ACME',
      strategy: 'none',
      days: ['2019-12-31'],
    };
    expect(apply(s, start).genDays).toEqual(['2019-12-31']);
    expect(apply(s, hello({ generation: 3 })).genDays).toEqual([]);
  });

  it("follows a Watch: its run from generation_start or state, its verdict, and the generation's spec", () => {
    const spec: Spec = {
      symbol: 'ACME',
      days: ['2019-12-30'],
      strategy: 'market_maker',
      params: {},
      latency: { order_ns: 50_000, report_ns: 50_000 },
      fees: { maker: -20, taker: 30 },
      limits: { max_position: 500, max_order_quantity: 100, max_open_orders: 2 },
      window: 'regular',
    };
    const start: ServerMessage = { ...generationStart(3, 1), run_id: 'r1' } as ServerMessage;
    const digests = { book: '1', execution: '2', portfolio: '3' };
    const verdict: ServerMessage = {
      type: 'report',
      kind: 'reproduced',
      session_id: 's1',
      generation: 3,
      seq: 9,
      run_id: 'r1',
      outcome: 'reproduced',
      digests,
      expected: digests,
    };
    const s = run(hello(), snapshot({ type: 'state' }), start);
    expect([s.runId, s.spec, s.verdict]).toEqual(['r1', null, null]);
    const watched = apply(apply(s, snapshot({ type: 'state', generation: 3, spec, run_id: 'r1' })), verdict);
    expect([watched.runId, watched.spec, watched.verdict]).toEqual(['r1', spec, verdict]);
    expect(apply(watched, snapshot({ generation: 3, snapshot_seq: 2 })).spec).toBe(spec); // a snapshot carries none
    const next = apply(watched, generationStart(4, 10));
    expect([next.runId, next.spec, next.verdict]).toEqual(['', null, null]);
  });

  it('keeps the days the day picker found when hello names a new session', () => {
    const days = [{ date: '2019-12-30' } as Day];
    expect(apply({ ...run(hello()), days }, hello({ session_id: 's2', generation: 1 })).days).toBe(days);
  });

  it('asks for a rebuilt chronos-serve when hello speaks an older protocol', () => {
    expect(run(hello({ protocol: 1 })).notice).toBe('chronos-serve speaks protocol 1: rebuild it');
    expect(run(hello()).notice).toBe('');
  });

  it('clears the notice on a control ack and a new generation, and the seek target when a seek starts', () => {
    const progress = (seek: number): ServerMessage => ({ type: 'progress', session_id: 's1', generation: 2, seek });
    const s = run(hello(), snapshot({ type: 'state' }), progress(T + 5 * S));
    const failed = apply(s, { type: 'error', session_id: 's1', generation: 2, code: 'not_ready', message: 'ended' });
    expect(failed.notice).toBe('not_ready: ended');
    expect(apply(failed, controlAck(1, 'play', 'playing')).notice).toBe('');
    expect(apply(failed, generationStart(3, 1)).notice).toBe('');
    expect(apply(failed, snapshot({ failure: 'source_error', source_error: 'eof' })).notice).not.toBe('');
    expect(s.seekAt).toBe(T + 5 * S);
    expect(apply(s, controlAck(1, 'speed', 'seeking')).seekAt).toBe(T + 5 * S); // another control: the seek goes on
    expect(apply(s, controlAck(1, 'seek', 'seeking')).seekAt).toBe(0);
    expect(apply(s, generationStart(3, 1)).seekAt).toBe(0); // a backward seek begins a generation
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

  it('ignores a stale_generation error that names no command: it answers a resync sent with an old session', () => {
    const s = run(hello(), snapshot({ type: 'state' }));
    const stale = (named: { client_id?: number; command_seq?: number } = {}): ServerMessage => ({
      type: 'error',
      session_id: 's1',
      generation: 2,
      code: 'stale_generation',
      message: 'old session',
      ...named,
    });
    expect(apply(s, stale())).toBe(s);
    expect(apply(s, stale({ client_id: 7 })).notice).toBe('order 7: stale_generation: old session');
    expect(apply(s, stale({ command_seq: 4 })).notice).toBe('stale_generation: old session');
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

  it('return one array for an unchanged list of resting orders, and a new one when any of its fields change', () => {
    const rest = (over: Partial<MyOrder> = {}): MyOrder => ({
      id: 1,
      side: 'buy',
      price: 100_000,
      remaining: 100,
      ahead_quantity: 250,
      ahead_orders: 2,
      ...over,
    });
    const first = run(hello(), snapshot({ type: 'state', my_orders: [rest()] }));
    const second = apply(first, snapshot({ snapshot_seq: 2, my_orders: [rest()] }));
    expect(second.snap?.my_orders).not.toBe(first.snap?.my_orders); // each snapshot parses its own
    expect(selectOrders(second)).toBe(selectOrders(first));
    expect(selectOrders(second)).toEqual([rest()]);
    const changes: MyOrder[][] = [
      [rest({ remaining: 99 })],
      [rest({ ahead_quantity: 249 })],
      [rest({ ahead_orders: 1 })],
      [rest({ price: 99_900 })],
      [rest({ side: 'sell' })],
      [rest({ id: 2 })],
      [rest(), rest({ id: 3 })],
      [],
    ];
    let seq = 2;
    for (const orders of changes) {
      const unchanged = selectOrders(second); // each change is measured from the same list
      const now = selectOrders(apply(second, snapshot({ snapshot_seq: ++seq, my_orders: orders })));
      expect(now).not.toBe(unchanged);
      expect(now).toEqual(orders);
      const again = apply(second, snapshot({ snapshot_seq: ++seq, my_orders: structuredClone(orders) }));
      expect(selectOrders(again)).toBe(now);
    }
  });

  it('allow trading only to a connected controller on a trading symbol that is neither seeking nor ended', () => {
    const s = { ...run(hello(), snapshot({ type: 'state' })), status: 'open' as const };
    expect(selectTradeBlock(s)).toBe('');
    expect(selectTradeBlock({ ...s, hello: hello({ role: 'viewer' }) })).not.toBe('');
    expect(selectTradeBlock({ ...s, status: 'closed' })).not.toBe('');
    expect(selectTradeBlock({ ...s, state: 'seeking' })).not.toBe('');
    expect(selectTradeBlock({ ...s, state: 'ended' })).not.toBe('');
    expect(selectTradeBlock({ ...s, snap: snapshot({ trading_state: 'halted' }) })).not.toBe('');
    expect(selectTradeBlock({ ...s, snap: snapshot({ symbol: '' }) })).not.toBe('');
  });

  it('say why trading is unavailable, for the ticket to show', () => {
    const s = { ...run(hello(), snapshot({ type: 'state' })), status: 'open' as const };
    expect(selectTradeBlock(s)).toBe('');
    expect(selectTradeBlock(initial)).toBe('Not connected');
    expect(selectTradeBlock({ ...s, status: 'closed' })).toBe('Not connected');
    expect(selectTradeBlock({ ...s, hello: hello({ role: 'viewer' }) })).toBe('Viewers cannot trade');
    expect(selectTradeBlock({ ...s, snap: null })).toBe('No symbol loaded');
    expect(selectTradeBlock({ ...s, snap: snapshot({ symbol: '' }) })).toBe('No symbol loaded');
    expect(selectTradeBlock({ ...s, snap: snapshot({ trading_state: 'halted' }) })).toBe('Not trading: halted');
    expect(selectTradeBlock({ ...s, state: 'seeking' })).toBe('Seeking');
    expect(selectTradeBlock({ ...s, snap: null, state: 'seeking' })).toBe('Seeking'); // a backward seek: no snapshot
    expect(selectTradeBlock({ ...s, state: 'ended' })).toBe('Session ended');
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
        if (path.startsWith('/api/days')) return Response.json({ start: '2019-12-30', days: [{ date: '2019-12-30' }] });
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
  // The from_ts of each history fetch so far.
  const asked = (f: ReturnType<typeof fake>) =>
    f.fetched
      .filter((path) => path.startsWith('/api/history'))
      .map((path) => Number(new URL(path, 'http://x').searchParams.get('from_ts')));
  const history = (candles: Candle[], trades: Trade[] = []) =>
    Response.json({ session_id: 's1', generation: 2, candles, trades });
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

  it('loads the days only when asked, and the symbols listed on every day picked', async () => {
    const f = fake();
    const store = createStore(f.env);
    await store.start();
    expect(f.fetched).toEqual(['/api/symbols']);
    await store.loadDays();
    expect(store.get().days.map((d) => d.date)).toEqual(['2019-12-30']);
    await store.loadSymbols(['2019-12-27', '2019-12-30']);
    expect(f.fetched.slice(1)).toEqual(['/api/days', '/api/symbols?days=2019-12-27,2019-12-30']);
    expect(store.get().symbols).toEqual(['ACME']);
  });

  it('shows a note the UI puts on the notice line', () => {
    const store = createStore(fake().env);
    store.notify('unknown symbol X');
    expect(store.get().notice).toBe('unknown symbol X');
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

  it('refetches only from the last closed bar after a skipped snapshot, merges, and rebuilds the chart only if it must', async () => {
    let reply = history([]);
    const f = fake({ history: () => reply });
    const store = await connected(
      f,
      hello(),
      snapshot({
        type: 'state',
        closed_candles: [bar(0, 1), bar(1, 1)],
        candle: bar(2, 1),
        trades: [
          [T, 1, 5, 'buy'],
          [T + S, 1, 10, 'buy'],
        ],
      }),
    );
    const refetched = async (n: number) => {
      await vi.waitFor(() => expect(f.histories()).toBe(n));
      await settle();
    };
    await refetched(1);

    // 2 was published and missed: bar 2 closed with other prices, bar 3 came and went, and a trade went by.
    reply = history(
      [bar(1, 1), bar(2, 9), bar(3, 9), bar(4, 1)],
      [
        [T + S, 1, 10, 'buy'],
        [T + 3 * S, 2, 30, 'sell'],
        [T + 4 * S, 3, 40, 'buy'],
      ],
    );
    let version = store.get().barsVersion;
    const missed = snapshot({
      snapshot_seq: 3,
      closed_candles: [bar(4, 1)],
      candle: bar(5, 1),
      trades: [[T + 4 * S, 3, 40, 'buy']],
    });
    f.socket().message(json(missed));
    await refetched(2);
    expect(asked(f)).toEqual([0, T + S]);
    expect(store.get().bars).toEqual([bar(0, 1), bar(1, 1), bar(2, 9), bar(3, 9), bar(4, 1), bar(5, 1)]);
    expect(store.get().tape.map((t) => t[2])).toEqual([5, 10, 30, 40]); // 5 is older than the history asked for
    expect(store.get().barsVersion).toBe(version + 1); // bars 2 and 3 lie before the chart's last

    // 4 was missed, but nothing before the open bar differs: the chart appends.
    reply = history([bar(4, 1)]);
    version = store.get().barsVersion;
    f.socket().message(json(snapshot({ snapshot_seq: 5, candle: bar(5, 1) })));
    await refetched(3);
    expect(asked(f)).toEqual([0, T + S, T + 4 * S]);
    expect(store.get().barsVersion).toBe(version);
  });

  it('refetches the whole generation after a state, replacing the bars', async () => {
    let reply = history([bar(1, 2)]);
    const f = fake({ history: () => reply });
    const store = await connected(f, hello(), snapshot({ type: 'state', closed_candles: [bar(0, 1)] }));
    await vi.waitFor(() => expect(store.get().bars).toEqual([bar(1, 2)])); // bar 0 is not in the history

    reply = history([bar(2, 3)]);
    f.socket().message(json(snapshot({ type: 'state', snapshot_seq: 2 })));
    await vi.waitFor(() => expect(store.get().bars).toEqual([bar(2, 3)]));
    expect(asked(f)).toEqual([0, 0]);
  });

  it('refetches from the last closed bar after a truncated snapshot, merging into the bars', async () => {
    let reply = history([]);
    const f = fake({ history: () => reply });
    const store = await connected(
      f,
      hello(),
      snapshot({ type: 'state', closed_candles: [bar(0, 1), bar(1, 1)], candle: bar(2, 1) }),
    );
    await vi.waitFor(() => expect(f.histories()).toBe(1));
    await settle();

    for (const [seq, over] of [
      [2, { candles_truncated: true }],
      [3, { trades_truncated: true }],
    ] as const) {
      reply = history([bar(1, 1), bar(2, 5)]);
      f.socket().message(json(snapshot({ snapshot_seq: seq, candle: bar(3, 1), ...over })));
      await vi.waitFor(() => expect(f.histories()).toBe(seq));
      await settle();
    }
    expect(asked(f)).toEqual([0, T + S, T + 2 * S]); // each time from the newest bar known closed
    expect(store.get().bars).toEqual([bar(0, 1), bar(1, 1), bar(2, 5), bar(3, 1)]);
  });

  it('refetches the whole generation after a truncated snapshot that begins a new generation', async () => {
    const f = fake({ history: echo });
    await connected(f, hello(), snapshot({ type: 'state', closed_candles: [bar(0, 1)], candle: bar(1, 1) }));
    await vi.waitFor(() => expect(f.histories()).toBe(1));
    await settle();
    f.socket().message(json(snapshot({ generation: 3, snapshot_seq: 2, trades_truncated: true })));
    await vi.waitFor(() => expect(f.histories()).toBe(2));
    expect(asked(f)).toEqual([0, 0]);
  });

  it('keeps the range a failed history fetch owed, so the next skip repairs from the older start', async () => {
    let offline = false;
    const f = fake({
      history: (path) => {
        if (offline) throw new Error('offline');
        return echo(path);
      },
    });
    await connected(f, hello(), snapshot({ type: 'state', closed_candles: [bar(0, 1)], candle: bar(1, 1) }));
    await vi.waitFor(() => expect(f.histories()).toBe(1));
    await settle();

    offline = true;
    f.socket().message(json(snapshot({ snapshot_seq: 3, closed_candles: [bar(1, 1)], candle: bar(2, 1) })));
    await vi.waitFor(() => expect(f.histories()).toBe(2));
    await settle();
    offline = false;
    f.socket().message(json(snapshot({ snapshot_seq: 5, closed_candles: [bar(3, 1)], candle: bar(4, 1) })));
    await vi.waitFor(() => expect(f.histories()).toBe(3));
    expect(asked(f)).toEqual([0, T, T]); // not T + S, where the last closed bar now is
  });

  it('lets a pending full refetch absorb the skips that come while one is in flight', async () => {
    let release!: () => void;
    const gate = new Promise<void>((resolve) => (release = resolve));
    const f = fake({
      history: async (path) => {
        await gate;
        return echo(path);
      },
    });
    await connected(f, hello(), snapshot({ type: 'state', closed_candles: [bar(0, 1)], candle: bar(1, 1) }));
    const feed = (over: Partial<Snapshot>) => f.socket().message(json(snapshot(over)));
    feed({ snapshot_seq: 3 }); // a skip: asks from T
    feed({ type: 'state', snapshot_seq: 4 }); // asks for everything
    feed({ snapshot_seq: 6 }); // another skip: still everything
    release();
    await vi.waitFor(() => expect(f.histories()).toBe(2));
    await settle();
    expect(asked(f)).toEqual([0, 0]);
  });

  it('rebuilds the chart when a skip finalises the open bar and nothing else, so no stale partial stays', async () => {
    let reply = history([]);
    const f = fake({ history: () => reply });
    const store = await connected(
      f,
      hello(),
      snapshot({ type: 'state', closed_candles: [bar(0, 1)], candle: bar(1, 1) }),
    );
    await vi.waitFor(() => expect(f.histories()).toBe(1));
    await settle();

    // 1 closed in a missed snapshot with other prices and volume; the history inserts no bar.
    reply = history([bar(0, 1), bar(1, 9, 700)]);
    const version = store.get().barsVersion;
    f.socket().message(json(snapshot({ snapshot_seq: 3, candle: bar(2, 1) })));
    await vi.waitFor(() => expect(f.histories()).toBe(2));
    await settle();
    expect(store.get().bars).toEqual([bar(0, 1), bar(1, 9, 700), bar(2, 1)]);
    expect(store.get().barsVersion).toBe(version + 1);
  });

  it('keeps a tape trade older than the fetch once when the history brings no trades', async () => {
    let reply = history([]);
    const f = fake({ history: () => reply });
    const store = await connected(
      f,
      hello(),
      snapshot({
        type: 'state',
        closed_candles: [bar(0, 1), bar(1, 1)],
        candle: bar(2, 1),
        trades: [[T, 1, 5, 'buy']], // after historyTs, before the skip's from_ts
      }),
    );
    await vi.waitFor(() => expect(f.histories()).toBe(1));
    await settle();

    reply = history([bar(1, 1)]); // no trades
    f.socket().message(json(snapshot({ snapshot_seq: 3, candle: bar(2, 1), trades: [[T + 4 * S, 3, 7, 'sell']] })));
    await vi.waitFor(() => expect(f.histories()).toBe(2));
    await settle();
    expect(asked(f)).toEqual([0, T + S]);
    expect(store.get().tape.map((t) => t[2])).toEqual([5, 7]);
  });
});
