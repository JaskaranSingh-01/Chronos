import { useSyncExternalStore } from 'react';
import type {
  Candle,
  Control,
  Execution,
  Hello,
  History,
  Order,
  ServerMessage,
  SessionState,
  Snapshot,
  Trade,
} from './protocol';

export type Status = 'connecting' | 'open' | 'closed' | 'token' | 'host';

export interface State {
  status: Status; // token: the server wants a token we lack; host: open the UI on 127.0.0.1 or localhost
  symbols: string[];
  hello: Hello | null;
  generation: number; // the newest seen in any envelope
  snap: Snapshot | null; // the latest of `generation`; null until its first state
  state: SessionState; // from snapshots and control acks
  seq: number; // the last ring seq applied: reports and control acks
  seekAt: number; // a running seek's exchange time
  fills: Execution[]; // this generation's, oldest first
  tape: Trade[]; // this generation's market trades, oldest first
  bars: Candle[]; // this generation's 1 s bars, oldest first
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
}

const tapeSize = 512;
const tokenKey = 'chronos-token';

export const initial: State = {
  status: 'connecting',
  symbols: [],
  hello: null,
  generation: 0,
  snap: null,
  state: 'ended',
  seq: 0,
  seekAt: 0,
  fills: [],
  tape: [],
  bars: [],
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

// History wins up to its last bar and trade; what snapshots brought after those stays.
export function withHistory(s: State, h: History): State {
  const lastBar = h.candles.at(-1)?.[0] ?? -1;
  const lastTrade = h.trades.at(-1)?.[0] ?? s.historyTs;
  return {
    ...s,
    bars: [...h.candles, ...s.bars.filter((b) => b[0] > lastBar)],
    barsVersion: s.barsVersion + 1,
    tape: [...h.trades, ...s.tape.filter((t) => t[0] > lastTrade)].slice(-tapeSize),
    historyTs: Math.max(s.historyTs, lastTrade),
  };
}

// A new generation starts with an empty book, account, chart and tape.
function newGeneration(s: State, generation: number): State {
  return {
    ...s,
    generation,
    snap: null,
    fills: [],
    tape: [],
    bars: [],
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

function onSnapshot(s: State, m: Snapshot): State {
  if (m.generation < s.generation) return s;
  const fresh = !s.snap || m.snapshot_seq > s.snap.snapshot_seq; // a reconnect's state may repeat the last snapshot
  // ponytail: trade times are epoch ns read as doubles (about 256 ns apart above 2^53), so two trades closer than that
  // compare equal and this cut-off can drop one; parse them as BigInt or strings if exactness matters.
  const trades = fresh ? m.trades.filter((t) => t[0] > s.historyTs) : [];
  return {
    ...s,
    snap: m,
    state: m.state,
    // A state with nothing to recover holds every report up to its watermark.
    seq: m.type === 'state' && (m.full_resync || s.seq === 0) ? Math.max(s.seq, m.last_report_seq) : s.seq,
    bars: fresh ? mergeBars(s.bars, m.candle ? [...m.closed_candles, m.candle] : m.closed_candles) : s.bars,
    tape: trades.length > 0 ? [...s.tape, ...trades].slice(-tapeSize) : s.tape,
    halts: halts(s, m),
    notice: m.failure !== 'none' ? `replay failed: ${m.failure} ${m.source_error}` : s.notice,
  };
}

export function apply(s: State, m: ServerMessage): State {
  if (m.type === 'hello') {
    const same = s.hello?.session_id === m.session_id; // a new id: the server restarted, and its seqs with it
    const base = same ? s : { ...initial, status: s.status, symbols: s.symbols, barsVersion: s.barsVersion + 1 };
    return { ...(m.generation > base.generation ? newGeneration(base, m.generation) : base), hello: m };
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
      return { ...s, seekAt: m.seek };
    case 'error':
      return { ...s, notice: `${m.client_id !== undefined ? `order ${m.client_id}: ` : ''}${m.code}: ${m.message}` };
    case 'ack':
      if (!('control' in m)) return s; // a command ack: logged; its verdict follows as lifecycle reports
      // A replay after a reconnect brings acks older than the state: they advance the cursor but not the state.
      if (m.seq <= s.seq) return s;
      return { ...s, seq: m.seq, state: m.seq > (s.snap?.last_report_seq ?? 0) ? m.state : s.state };
    case 'report': {
      if (m.seq <= s.seq) return s;
      const next = { ...s, seq: m.seq };
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
// Every snapshot parses its own empty array, so an empty list maps to one constant to keep the panel from re-rendering.
export const selectOrders = (s: State) => (s.snap && s.snap.my_orders.length > 0 ? s.snap.my_orders : none);
// Manual trading: the controller, connected, on a symbol that is trading, neither seeking nor ended.
export const selectCanTrade = (s: State) =>
  s.hello?.role === 'controller' &&
  s.status === 'open' &&
  s.snap !== null &&
  s.snap.symbol !== '' &&
  s.snap.trading_state === 'trading' &&
  s.state !== 'seeking' &&
  s.state !== 'ended';

export function createStore(env: Env) {
  let state = initial;
  let token = '';
  let clientId = 0;
  let socket: { send(text: string): void } | null = null;
  const listeners = new Set<() => void>();

  const set = (next: State) => {
    state = next;
    listeners.forEach((f) => f());
  };
  const api = (path: string) => env.fetch(path, token ? { headers: { Authorization: `Bearer ${token}` } } : undefined);

  // ponytail: the whole generation each time (about 6 MB for a busy day); fetch from the last bar if that gets slow.
  async function fetchHistory() {
    try {
      const { hello, generation } = state;
      if (!hello) return;
      const res = await api(
        `/api/history?session_id=${encodeURIComponent(hello.session_id)}&generation=${generation}&from_ts=0`,
      );
      if (!res.ok) return; // 409: a newer generation, whose own state fetches again
      const h = (await res.json()) as History;
      if (h.session_id === state.hello?.session_id && h.generation === state.generation) set(withHistory(state, h));
    } catch {
      // A network or parse error marks nothing; the next trigger fetches again.
    }
  }

  // One fetch in flight, and one more if a trigger came meanwhile: it reads the newest generation when it starts.
  let loading = false;
  let again = false;
  async function loadHistory() {
    again = true;
    if (loading) return;
    loading = true;
    try {
      while (again) {
        again = false;
        await fetchHistory();
      }
    } finally {
      loading = false;
    }
  }

  function receive(text: string) {
    const m = JSON.parse(text) as ServerMessage;
    const prev = state.snap;
    set(apply(state, m));
    // After a state, and whenever a snapshot left trades or bars out, by a truncation flag or a skipped snapshot_seq
    // (a slow poll misses published snapshots); only if it was applied.
    if ((m.type === 'state' || m.type === 'snapshot') && state.snap === m) {
      const skipped = prev !== null && prev.generation === m.generation && m.snapshot_seq > prev.snapshot_seq + 1;
      if (m.type === 'state' || m.trades_truncated || m.candles_truncated || skipped) void loadHistory();
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

  // Finds out whether the server wants a token, loads the symbols, then opens the WebSocket; one at a time, since an
  // orphaned socket would keep the controller seat.
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
    receive,
    setToken(value: string) {
      env.save(tokenKey, value);
      void start();
    },
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
});

// A panel's slice of the state; it re-renders only when that slice changes, so selectors return stable references.
export function useStore<T>(select: (s: State) => T): T {
  return useSyncExternalStore(store.subscribe, () => select(store.get()));
}
