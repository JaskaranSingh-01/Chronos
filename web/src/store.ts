import { useSyncExternalStore } from 'react';
import type {
  Candle,
  Control,
  Day,
  Execution,
  Hello,
  History,
  MyOrder,
  Order,
  Reproduced,
  ServerMessage,
  SessionState,
  Snapshot,
  Spec,
  Trade,
} from './protocol';

export type Status = 'connecting' | 'open' | 'closed' | 'token' | 'host';

export interface State {
  status: Status; // token: the server wants a token we lack; host: open the UI on 127.0.0.1 or localhost
  symbols: string[]; // listed on every day picked
  days: Day[]; // every day the server found, as of the day picker's last look
  hello: Hello | null;
  generation: number; // the newest seen in any envelope
  symbol: string; // this generation's, from its generation_start or a snapshot; '' until known
  genDays: string[]; // this generation's days, from its generation_start or state
  spec: Spec | null; // this generation's settings, from its state: a Watch brings its run's fees and limits
  runId: string; // the run this generation watches, from its generation_start or state; '' for none
  verdict: Reproduced | null; // the Watch's, once its generation has ended
  snap: Snapshot | null; // the latest of `generation`; null until its first state
  state: SessionState; // from snapshots and control acks
  seq: number; // the last ring seq applied: reports and control acks
  seekAt: number; // a running seek's exchange time
  fills: Execution[]; // this generation's, oldest first
  tape: Trade[]; // this generation's market trades, oldest first
  bars: Candle[]; // this generation's 1 s bars, oldest first
  pnl: [number, number][]; // the account's PnL at each exchange second this page saw a snapshot, oldest first
  barsVersion: number; // bumped when `bars` is rebuilt rather than extended
  historyTs: number; // the last history trade: snapshot trades at or before it are already in the tape
  halts: [number, number][]; // exchange-time spans the book was seen not trading
  historyIncomplete: boolean; // reports were lost past the server's ring
  notice: string; // the last error or rejection
}

export interface Env {
  fetch(path: string, init?: RequestInit): Promise<Response>;
  connect(url: string, on: { open(): void; message(text: string): void; close(): void }): { send(text: string): void };
  load(key: string): string | null;
  save(key: string, value: string): void;
  host(): string;
  frame(f: () => void): void; // runs `f` before the next paint
}

const tapeSize = 512;
export const tokenKey = 'chronos-token'; // sessionStorage: the server's token, for the socket and the Lab
const protocol = 2; // the server protocol this terminal speaks

export const initial: State = {
  status: 'connecting',
  symbols: [],
  days: [],
  hello: null,
  generation: 0,
  symbol: '',
  genDays: [],
  spec: null,
  runId: '',
  verdict: null,
  snap: null,
  state: 'ended',
  seq: 0,
  seekAt: 0,
  fills: [],
  tape: [],
  bars: [],
  pnl: [],
  barsVersion: 0,
  historyTs: 0,
  halts: [],
  historyIncomplete: false,
  notice: '',
};

// `bars` and `incoming`, both oldest first, merged by start: a known start is replaced, a new one inserted in order.
export function mergeBars(bars: Candle[], incoming: readonly Candle[]): Candle[] {
  if (incoming.length === 0) return bars;
  const out = bars.slice();
  for (const bar of incoming) {
    let i = out.length;
    while (i > 0 && out[i - 1][0] > bar[0]) i--;
    if (i > 0 && out[i - 1][0] === bar[0]) out[i - 1] = bar;
    else out.splice(i, 0, bar);
  }
  return out;
}

// Whether merging `incoming` into `bars` inserts or changes a bar before the last one: the chart cannot append those.
// ponytail: indexes every bar per partial merge (~23k a day, under 1 ms); bound it by the first incoming start.
function reachesBack(bars: readonly Candle[], incoming: readonly Candle[]): boolean {
  const last = bars.at(-1)?.[0] ?? -1;
  const known = new Map(bars.map((b) => [b[0], b]));
  return incoming.some((c) => c[0] < last && !known.get(c[0])?.every((x, i) => x === c[i]));
}

// History from `from` on (the whole generation when 0) wins up to its last bar and trade; later snapshot data stays.
export function withHistory(s: State, h: History, from = 0): State {
  const lastBar = h.candles.at(-1)?.[0] ?? -1;
  const lastTrade = h.trades.at(-1)?.[0] ?? s.historyTs;
  return {
    ...s,
    bars: from === 0 ? [...h.candles, ...s.bars.filter((b) => b[0] > lastBar)] : mergeBars(s.bars, h.candles),
    barsVersion: from === 0 || reachesBack(s.bars, h.candles) ? s.barsVersion + 1 : s.barsVersion,
    tape: [
      ...s.tape.filter((t) => t[0] < from),
      ...h.trades,
      ...s.tape.filter((t) => t[0] >= from && t[0] > lastTrade),
    ].slice(-tapeSize),
    historyTs: Math.max(s.historyTs, lastTrade),
  };
}

// The start of the last bar known closed in `s`, or 0: a skipped snapshot loses the bars after it.
const lastClosed = (s: State) => s.bars.findLast((b) => b[0] !== s.snap?.candle?.[0])?.[0] ?? 0;

// A new generation starts with an empty book, account, chart and tape.
function newGeneration(s: State, generation: number): State {
  return {
    ...s,
    generation,
    symbol: '',
    genDays: [],
    spec: null,
    runId: '',
    verdict: null,
    snap: null,
    seekAt: 0,
    notice: '',
    fills: [],
    tape: [],
    bars: [],
    pnl: [],
    barsVersion: s.barsVersion + 1,
    historyTs: 0,
    halts: [],
    historyIncomplete: false,
  };
}

// The book's non-trading spans: a snapshot that is not trading extends the span its predecessor opened.
// ponytail: only spans this page saw; a reload forgets earlier halts, which the history would have to carry.
function halts(s: State, m: Snapshot): [number, number][] {
  if (m.trading_state === 'trading') return s.halts;
  const last = s.halts.at(-1);
  return last && s.snap?.trading_state !== 'trading'
    ? [...s.halts.slice(0, -1), [last[0], m.exchange_ts]]
    : [...s.halts, [m.exchange_ts, m.exchange_ts]];
}

// One PnL sample a second: a snapshot in the same exchange second replaces the last.
// ponytail: only what this page saw (the history carries no PnL); copies the curve per snapshot, ~57,600 a full day.
function samplePnl(pnl: State['pnl'], m: Snapshot): State['pnl'] {
  const last = pnl.at(-1);
  const sameSecond = last !== undefined && Math.floor(last[0] / 1e9) === Math.floor(m.exchange_ts / 1e9);
  if (sameSecond && last[1] === m.account.pnl) return pnl;
  return [...(sameSecond ? pnl.slice(0, -1) : pnl), [m.exchange_ts, m.account.pnl]];
}

function onSnapshot(s: State, m: Snapshot): State {
  if (m.generation < s.generation) return s;
  const fresh = !s.snap || m.snapshot_seq > s.snap.snapshot_seq; // a reconnect's state may repeat the last snapshot
  // ponytail: trade times are epoch ns doubles (~256 ns apart); this cut-off can drop a close pair, BigInt if exact.
  const trades = fresh ? m.trades.filter((t) => t[0] > s.historyTs) : [];
  return {
    ...s,
    snap: m,
    symbol: m.symbol,
    genDays: m.spec?.days ?? s.genDays,
    spec: m.spec ?? s.spec,
    runId: m.run_id ?? s.runId,
    state: m.state,
    // A state with nothing to recover holds every report up to its watermark.
    seq: m.type === 'state' && (m.full_resync || s.seq === 0) ? Math.max(s.seq, m.last_report_seq) : s.seq,
    bars: fresh ? mergeBars(s.bars, m.candle ? [...m.closed_candles, m.candle] : m.closed_candles) : s.bars,
    tape: trades.length > 0 ? [...s.tape, ...trades].slice(-tapeSize) : s.tape,
    pnl: fresh && m.symbol !== '' ? samplePnl(s.pnl, m) : s.pnl,
    halts: halts(s, m),
    notice: m.failure !== 'none' ? `replay failed: ${m.failure} ${m.source_error}` : s.notice,
  };
}

export function apply(s: State, m: ServerMessage): State {
  if (m.type === 'hello') {
    const same = s.hello?.session_id === m.session_id; // a new id: the server restarted, and its seqs with it
    const base = same
      ? s
      : { ...initial, status: s.status, symbols: s.symbols, days: s.days, barsVersion: s.barsVersion + 1 };
    const next = { ...(m.generation > base.generation ? newGeneration(base, m.generation) : base), hello: m };
    // An older server is a stale binary, met only through the Vite dev proxy.
    return m.protocol < protocol
      ? { ...next, notice: `chronos-serve speaks protocol ${m.protocol}: rebuild it` }
      : next;
  }
  if (m.session_id !== s.hello?.session_id) return s; // from a session this client no longer follows
  if (m.generation > s.generation) s = newGeneration(s, m.generation);
  switch (m.type) {
    case 'state':
    case 'snapshot':
      return onSnapshot(s, m);
    case 'gap':
      return { ...s, seq: Math.max(s.seq, m.to), historyIncomplete: true };
    case 'progress':
      return { ...s, seekAt: m.seek, state: 'seeking' }; // sent only while seeking: fixes a stale state on reconnect
    case 'error':
      // The answer to a resync sent with an old session after a restart: nothing of ours failed.
      if (m.code === 'stale_generation' && m.client_id === undefined && m.command_seq === undefined) return s;
      return { ...s, notice: `${m.client_id !== undefined ? `order ${m.client_id}: ` : ''}${m.code}: ${m.message}` };
    case 'ack':
      if (!('control' in m)) return s; // a command ack: logged; its verdict follows as lifecycle reports
      // A replay after a reconnect brings acks older than the state: they advance the cursor but not the state.
      if (m.seq <= s.seq) return s;
      return {
        ...s,
        seq: m.seq,
        state: m.seq > (s.snap?.last_report_seq ?? 0) ? m.state : s.state,
        seekAt: m.control === 'seek' ? 0 : s.seekAt, // the last seek's target is not this one's
        notice: '', // a control the server applied answers an earlier refusal
      };
    case 'report': {
      if (m.seq <= s.seq) return s;
      const next = { ...s, seq: m.seq };
      if (m.kind === 'generation_start' && m.generation === s.generation) {
        next.symbol = m.symbol;
        next.genDays = m.days ?? s.genDays;
        next.runId = m.run_id ?? s.runId;
      }
      if (m.kind === 'reproduced' && m.generation === s.generation) next.verdict = m;
      if (m.kind === 'execution' && m.report_type === 'fill' && m.generation === s.generation) {
        next.fills = [...s.fills, m];
      }
      if (m.kind === 'lifecycle' && m.reason !== 'none') {
        next.notice = `${m.command}${m.command === 'order' ? ` ${m.client_id}` : ''} ${m.step}: ${m.reason}`;
      }
      return next;
    }
    default:
      return s;
  }
}

const none: never[] = [];
let lastOrders: MyOrder[] = none;
const sameOrder = (a: MyOrder, b: MyOrder) =>
  a.id === b.id &&
  a.side === b.side &&
  a.price === b.price &&
  a.remaining === b.remaining &&
  a.ahead_quantity === b.ahead_quantity &&
  a.ahead_orders === b.ahead_orders;
// Each snapshot parses a new array: an empty list maps to a constant and an unchanged one to the last array returned.
export function selectOrders(s: State): MyOrder[] {
  const next = s.snap?.my_orders ?? none;
  if (next.length === 0) return none;
  if (next.length !== lastOrders.length || !next.every((o, i) => sameOrder(o, lastOrders[i]))) lastOrders = next;
  return lastOrders;
}
// Why manual trading is unavailable, or '' when it is not: the controller, connected, not seeking or watching a run,
// on a trading symbol.
// ponytail: a strategy attached outside a Watch is not visible to the client; the server's not_ready is its signal.
export function selectTradeBlock(s: State): string {
  if (!s.hello || s.status !== 'open') return 'Not connected';
  if (s.hello.role !== 'controller') return 'Viewers cannot trade';
  if (s.state === 'seeking') return 'Seeking';
  if (s.runId) return "The watched run's strategy trades";
  if (!s.snap || s.snap.symbol === '') return 'No symbol loaded';
  if (s.snap.trading_state !== 'trading') return `Not trading: ${s.snap.trading_state}`;
  if (s.state === 'ended') return 'Session ended';
  return '';
}

export function createStore(env: Env) {
  let state = initial;
  let token = '';
  let clientId = 0;
  let socket: { send(text: string): void } | null = null;
  const listeners = new Set<() => void>();

  // The state changes with every message, but its listeners hear of it once a frame: a run watched at Max sends
  // thousands of messages a second, and the screen shows 60.
  let notifying = false;
  const set = (next: State) => {
    state = next;
    if (notifying) return;
    notifying = true;
    env.frame(() => {
      notifying = false;
      listeners.forEach((f) => f());
    });
  };
  const api = (path: string) => env.fetch(path, token ? { headers: { Authorization: `Bearer ${token}` } } : undefined);

  // False if the fetch failed and its range is still owed.
  // ponytail: a state or new generation refetches it all (~6 MB a busy day); a finer cut needs the server.
  async function fetchHistory(from: number): Promise<boolean> {
    try {
      const { hello, generation } = state;
      if (!hello) return true;
      const res = await api(
        `/api/history?session_id=${encodeURIComponent(hello.session_id)}&generation=${generation}&from_ts=${from}`,
      );
      if (!res.ok) return res.status === 409; // 409: a newer generation, whose own state fetches again
      const h = (await res.json()) as History;
      if (h.session_id === state.hello?.session_id && h.generation === state.generation) {
        set(withHistory(state, h, from));
      }
      return true;
    } catch {
      return false;
    }
  }

  // One fetch in flight, one pending; `owed` is the earliest bar start still to fetch (0: all), kept if a fetch fails.
  let loading = false;
  let again = false;
  let owed: number | null = null;
  async function loadHistory(from: number) {
    owed = Math.min(owed ?? from, from);
    again = true;
    if (loading) return;
    loading = true;
    try {
      while (again) {
        again = false;
        const asked = owed!;
        owed = null;
        if (!(await fetchHistory(asked))) owed = Math.min(owed ?? asked, asked);
      }
    } finally {
      loading = false;
    }
  }

  function receive(text: string) {
    const m = JSON.parse(text) as ServerMessage;
    const before = state;
    set(apply(state, m));
    // A state or a new generation refetches all; a truncation or a skipped snapshot_seq, from the last closed bar.
    if ((m.type === 'state' || m.type === 'snapshot') && state.snap === m) {
      const prev = before.snap;
      const sameGeneration = prev !== null && prev.generation === m.generation;
      const truncated = m.trades_truncated || m.candles_truncated;
      const skipped = sameGeneration && m.snapshot_seq > prev.snapshot_seq + 1;
      if (m.type === 'state' || (truncated && !sameGeneration)) void loadHistory(0);
      else if (truncated || skipped) void loadHistory(lastClosed(before));
    }
  }

  function open() {
    const ws = env.connect(`ws://${env.host()}/ws`, {
      open() {
        if (token) ws.send(JSON.stringify({ type: 'auth', token }));
        // Before hello, so no report the server sends from the new state's watermark can overtake the replay.
        if (state.hello && state.seq > 0) {
          ws.send(
            JSON.stringify({
              type: 'resync',
              session_id: state.hello.session_id,
              generation: state.generation,
              from_report_seq: state.seq + 1,
            }),
          );
        }
        set({ ...state, status: 'open' });
      },
      message(text) {
        if (socket === ws) receive(text);
      },
      close() {
        if (socket !== ws) return;
        socket = null;
        set({ ...state, status: 'closed' });
        setTimeout(start, 1_000);
      },
    });
    socket = ws;
  }

  // Checks for a token, loads the symbols, then opens the socket; one at a time, as an orphaned socket keeps the seat.
  let starting = false;
  async function start() {
    if (starting || socket) return;
    starting = true;
    try {
      let res = await env.fetch('/api/symbols');
      token = res.status === 401 ? (env.load(tokenKey) ?? '') : '';
      if (token) res = await api('/api/symbols');
      if (res.status === 401 || res.status === 403)
        return set({ ...state, status: res.status === 401 ? 'token' : 'host' });
      set({ ...state, symbols: ((await res.json()) as { symbols: string[] }).symbols });
      open();
    } catch {
      set({ ...state, status: 'closed' });
      setTimeout(start, 1_000);
    } finally {
      starting = false;
    }
  }

  // The days the server finds now: it rescans its directory for each ask.
  async function loadDays() {
    try {
      const res = await api('/api/days');
      if (res.ok) set({ ...state, days: ((await res.json()) as { days: Day[] }).days });
    } catch {
      // the list stays as it was; the next look asks again
    }
  }

  // The symbols listed on every one of `days`; only the latest ask's answer is kept.
  let symbolDays = '';
  async function loadSymbols(days: string[]) {
    const asked = (symbolDays = days.join(','));
    try {
      const res = await api(`/api/symbols?days=${asked}`);
      const symbols = res.ok ? ((await res.json()) as { symbols: string[] }).symbols : null;
      if (symbols && asked === symbolDays) set({ ...state, symbols });
    } catch {
      // the list stays as it was; the next pick asks again
    }
  }

  // Never queued: a message the socket cannot take now is refused, so nothing stale is sent after a reconnect.
  function send(body: object): boolean {
    const hello = state.hello;
    if (!socket || state.status !== 'open' || !hello) {
      set({ ...state, notice: 'not connected' });
      return false;
    }
    socket.send(JSON.stringify({ ...body, session_id: hello.session_id, generation: state.generation }));
    return true;
  }

  return {
    get: () => state,
    subscribe(f: () => void) {
      listeners.add(f);
      return () => listeners.delete(f);
    },
    start,
    setToken(value: string) {
      env.save(tokenKey, value);
      void start();
    },
    notify: (notice: string) => set({ ...state, notice }),
    loadDays,
    loadSymbols,
    control: (c: Control) => send({ type: 'control', ...c }),
    order: (o: Order) => send({ type: 'order', client_id: ++clientId, ...o }),
    cancel: (order_id: number) => send({ type: 'cancel', order_id }),
  };
}

export const store = createStore({
  fetch: (path, init) => fetch(path, init),
  connect(url, on) {
    const ws = new WebSocket(url);
    ws.onopen = on.open;
    ws.onmessage = (e) => on.message(String(e.data));
    ws.onclose = on.close;
    return ws;
  },
  load: (key) => sessionStorage.getItem(key),
  save: (key, value) => sessionStorage.setItem(key, value),
  host: () => location.host,
  frame: (f) => void requestAnimationFrame(f),
});

// A controller that has not loaded a symbol yet: generation 1 is the empty ticker.
export const selectFirstRun = (s: State) => s.hello?.role === 'controller' && s.generation <= 1;

// A panel's slice of the state; it re-renders only when that slice changes, so selectors return stable references.
export function useStore<T>(select: (s: State) => T): T {
  return useSyncExternalStore(store.subscribe, () => select(store.get()));
}
