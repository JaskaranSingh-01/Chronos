import { useSyncExternalStore } from 'react';
import { px } from './format';
import { signedTicks } from './order';
import type { Day, Digests, FillRule, Outcome, Spec } from './protocol';
import { tokenKey, type Env } from './store';

// chronos-serve's Lab over HTTP (server/src/protocol.cpp): money in tick-shares, times in epoch ns but equity.t.

export type RunState = 'queued' | 'running' | 'completed' | 'failed' | 'cancelled';
export type StrategyKind = 'market_maker' | 'imbalance';
export type Window = 'regular' | 'full_day';
export interface Limits {
  max_position: number;
  max_order_quantity: number;
  max_open_orders: number;
}

// GET /api/strategies: each strategy's params with their defaults and caps, and the run's other caps.
export interface Strategies {
  strategies: {
    kind: StrategyKind;
    params: { name: string; default: number; min: number; max: number | null }[]; // max null: any finite number
    limits: Limits; // at the default params; an omitted limit follows the params
  }[];
  latency: { order_ns: number; report_ns: number; max: number };
  fees: { maker: number; taker: number; min: number; max: number; taker_min: number }; // ticks a share
  limit_max: number;
  windows: { id: Window; label: string }[];
  max_days: number;
  max_queued: number;
}

// POST /api/backtests: symbol, days and strategy are required; any other key, or key inside one, takes its default.
export interface RunSpecBody {
  symbol: string;
  days: string[];
  strategy: StrategyKind;
  params?: Record<string, number>;
  latency?: { order_ns?: number; report_ns?: number };
  fees?: { maker?: number; taker?: number };
  limits?: Partial<Limits>;
  window?: Window;
}

// A refusal: chronos-serve's 4xx body, an HTTP status with none, or 'unreachable'.
export interface LabError {
  error: string;
  field?: string; // bad_request: the first bad key, e.g. 'params.quote_size' or 'days[1]'; omitted when none is named
  days?: string[]; // symbol_missing: every day whose directory lacks the symbol
}

// The run form as typed: numbers as text, fees in dollars a share, and an empty limit following the params.
export interface Form {
  strategy: StrategyKind;
  symbol: string;
  days: string[];
  window: Window;
  params: Record<string, string>;
  latency: { order_ns: string; report_ns: string };
  fees: { maker: string; taker: string };
  limits: Record<keyof Limits, string>;
}

export interface Verification {
  finished: string; // UTC, e.g. 2026-10-20T05:17:30Z
  outcome: Outcome;
  digests: Digests | null; // null when the re-run failed
  source_digest: string; // the verifying build's
  changed: string[]; // input_changed: the days whose re-hash differs
}

export interface Results {
  partial: boolean; // failed or cancelled: the aggregates so far
  events: number;
  sim_events: number;
  pnl: number;
  realised: number;
  unrealised: number;
  fees: number; // paid; a rebate is negative
  position: number;
  max_abs_position: number;
  max_drawdown: number; // the Portfolio's, exact at every mark
  fills: number;
  maker_fills: number;
  taker_fills: number;
  filled_shares: number;
  fill_rate: number; // filled / (filled + cancelled + expired) shares
  digests: Digests | null; // null when partial
  equity: { granularity: 'event_second'; t: number[]; pnl: number[]; position: number[] }; // t: epoch seconds
  by_hour: { hour: number[]; pnl: number[] }; // New York hour, summed over days
  by_day: { date: string; pnl: number; fills: number }[];
  attribution: { rules: FillRule[]; phases: string[]; fills: number[][]; anomalies: number[][] }; // [rule][phase]
  fill_list: {
    truncated: boolean; // more than 50,000 fills
    rows: [
      ts: number,
      order_id: number,
      side: 'buy' | 'sell',
      price: number,
      qty: number,
      liquidity: 'maker' | 'taker',
      rule: FillRule | 'none',
      ahead_at_entry: number,
      position: number,
    ][];
  };
}

// A Lab run, GET /api/backtests/<id>.
export interface Run {
  format: 1;
  id: string;
  state: RunState;
  reason: string; // failed: e.g. 'source_error on 2019-12-27: TruncatedMessage'
  warning: string; // e.g. 'not saved: No space left on device'
  spec: Spec; // every default filled in
  provenance: {
    build: {
      commit: string;
      dirty: boolean;
      source_digest: string;
      compiler: string;
      build_type: string;
      sanitize: string;
      platform: string;
    };
    engine: { book_capacity: number; price_scale: number; quantity_scale: number; check_invariants_every: number };
    allow_truncated_tail: boolean;
    days: {
      date: string;
      file: string;
      size: number;
      hash: string;
      session: { open: number; close: number; source: 'feed' | 'fallback' };
    }[];
    host: string;
    submitted: string;
    started: string; // '' until it starts
    finished: string;
    wall_ms: number;
    cpu_ms: number;
  };
  results: Results | null; // null while queued or running
  verifying: 'queued' | 'running' | null;
  verifications: Verification[];
  reproduced: boolean | null; // null until a verification on the same code and days completes
}

// A run as GET /api/backtests lists it: without its results.
export interface RunListing extends Omit<Run, 'results'> {
  progress: number; // 0..1, the bytes read
  partial: boolean;
  pnl: number | null;
  fills: number | null;
}

export interface LabState {
  strategies: Strategies | null; // null until chronos-serve answers
  start: string; // the starting day, the form's first
  days: Day[]; // every day the server found, as of the last look
  symbols: string[]; // listed on every day the form picks
  runs: RunListing[]; // oldest first: run #1 is the first
  offline: boolean; // the last listing failed; polling goes on until one answers
  selected: string; // the run shown; '' for none
  details: Record<string, Run>; // the shown run's JSON, kept until its state or its verifications change
  notice: string; // a refused cancel or delete, or a delete's warning
}

export const strategyNames: Record<string, string> = { market_maker: 'Market maker', imbalance: 'Imbalance' };

// The page a location hash shows: the Lab at #lab, else the terminal.
export const page = (hash: string) => (hash === '#lab' ? 'lab' : 'terminal');

// A run's money decimals: tick-shares are 10^(price_scale + quantity_scale) a dollar.
export const moneyDigits = (r: Pick<Run, 'provenance'>) =>
  r.provenance.engine.price_scale + r.provenance.engine.quantity_scale;

export const windowLabels: Record<Window, string> = { regular: 'Regular (feed session)', full_day: 'Full day' };

// The form for `kind` at the defaults /api/strategies serves; fees in dollars at `digits` decimals.
export function defaults(s: Strategies, kind: StrategyKind, digits: number, days: string[]): Form {
  const strategy = s.strategies.find((k) => k.kind === kind)!;
  return {
    strategy: kind,
    symbol: '',
    days,
    window: 'regular',
    params: Object.fromEntries(strategy.params.map((p) => [p.name, String(p.default)])),
    latency: { order_ns: String(s.latency.order_ns), report_ns: String(s.latency.report_ns) },
    fees: { maker: px(s.fees.maker, digits), taker: px(s.fees.taker, digits) },
    limits: { max_position: '', max_order_quantity: '', max_open_orders: '' },
  };
}

// A run's settings as the form, for "Edit as new".
export const fromSpec = (spec: Spec, digits: number): Form => ({
  strategy: spec.strategy as StrategyKind,
  symbol: spec.symbol,
  days: spec.days,
  window: spec.window,
  params: Object.fromEntries(Object.entries(spec.params).map(([name, value]) => [name, String(value)])),
  latency: { order_ns: String(spec.latency.order_ns), report_ns: String(spec.latency.report_ns) },
  fees: { maker: px(spec.fees.maker, digits), taker: px(spec.fees.taker, digits) },
  limits: {
    max_position: String(spec.limits.max_position),
    max_order_quantity: String(spec.limits.max_order_quantity),
    max_open_orders: String(spec.limits.max_open_orders),
  },
});

const number = (text: string) => (text.trim() === '' ? NaN : Number(text));

// The run the form describes, or the first field past its cap, named as chronos-serve names it.
export function toBody(f: Form, s: Strategies, digits: number): RunSpecBody | string {
  const kind = s.strategies.find((k) => k.kind === f.strategy)!;
  const params: Record<string, number> = {};
  for (const p of kind.params) {
    const value = number(f.params[p.name] ?? '');
    if (!(value >= p.min && value <= (p.max ?? Infinity))) return `params.${p.name}`;
    params[p.name] = value;
  }
  if (f.symbol.trim() === '') return 'symbol';
  if (f.days.length === 0) return 'days';
  const latency = { order_ns: number(f.latency.order_ns), report_ns: number(f.latency.report_ns) };
  for (const [key, value] of Object.entries(latency)) {
    if (!Number.isSafeInteger(value) || value < 0 || value > s.latency.max) return `latency.${key}`;
  }
  const fees = { maker: signedTicks(f.fees.maker, digits), taker: signedTicks(f.fees.taker, digits) };
  if (!(fees.maker >= s.fees.min && fees.maker <= s.fees.max)) return 'fees.maker';
  if (!(fees.taker >= s.fees.taker_min && fees.taker <= s.fees.max)) return 'fees.taker';
  const limits: Partial<Limits> = {};
  for (const [key, text] of Object.entries(f.limits) as [keyof Limits, string][]) {
    if (text.trim() === '') continue;
    const value = Number(text);
    if (!Number.isSafeInteger(value) || value < 0 || value > s.limit_max) return `limits.${key}`;
    limits[key] = value;
  }
  const symbol = f.symbol.trim().toUpperCase();
  return { symbol, days: f.days, strategy: f.strategy, params, latency, fees, limits, window: f.window };
}

// Why chronos-serve refused a run, for the form.
export function refusalText(r: LabError, symbol: string): string {
  if (r.error === 'symbol_missing') return `${symbol} is not listed on ${r.days?.join(', ')}`;
  if (r.error === 'bad_request' && r.field) return `Check ${r.field}`;
  if (r.error === 'queue_full') return 'Refused: the queue is full';
  if (r.error === 'unreachable') return 'chronos-serve does not answer';
  return `Refused: ${r.error}`;
}

// What a listed run allows.
export const actions = (r: RunListing) => ({ cancel: r.state === 'queued' || r.state === 'running' });

export function createLab(env: Pick<Env, 'fetch' | 'load'>) {
  let state: LabState = {
    strategies: null,
    start: '',
    days: [],
    symbols: [],
    runs: [],
    offline: false,
    selected: '',
    details: {},
    notice: '',
  };
  const listeners = new Set<() => void>();
  const set = (next: LabState) => {
    state = next;
    listeners.forEach((f) => f());
  };
  const headers = (): Record<string, string> => {
    const token = env.load(tokenKey);
    return token ? { Authorization: `Bearer ${token}` } : {};
  };
  const get = (path: string) => env.fetch(path, { headers: headers() });
  // A POST or DELETE: JSON, which chronos-serve wants of every change, as no other site's form can send it.
  const send = (method: string, path: string, body: object = {}) =>
    env.fetch(path, {
      method,
      headers: { ...headers(), 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });

  // The days chronos-serve finds now: it rescans its directory for each ask.
  async function loadDays() {
    try {
      const res = await get('/api/days');
      const found = res.ok ? ((await res.json()) as { start: string; days: Day[] }) : null;
      if (found) set({ ...state, start: found.start, days: found.days });
    } catch {
      // the list stays as it was; the next look asks again
    }
  }

  // The symbols listed on every one of `days`; only the latest ask's answer is kept.
  let symbolDays = '';
  async function loadSymbols(days: string[]) {
    const asked = (symbolDays = days.join(','));
    try {
      const res = await get(`/api/symbols?days=${asked}`);
      const symbols = res.ok ? ((await res.json()) as { symbols: string[] }).symbols : null;
      if (symbols && asked === symbolDays) set({ ...state, symbols });
    } catch {
      // the list stays as it was; the next pick asks again
    }
  }

  // The shown run's JSON, unless the copy kept is of the state and the verifications listed.
  async function fetchRun(id: string) {
    const listed = state.runs.find((r) => r.id === id);
    const kept = state.details[id];
    if (!listed || (kept?.state === listed.state && kept.verifications.length === listed.verifications.length)) return;
    try {
      const res = await get(`/api/backtests/${id}`);
      const run = res.ok ? ((await res.json()) as Run) : null;
      if (run && id === state.selected) set({ ...state, details: { [id]: run } });
    } catch {
      // the next listing asks again
    }
  }

  // Lists the runs, again at once if asked meanwhile; then each second while a run or a verification is queued or
  // running, or while chronos-serve does not answer.
  let listing = false;
  let again = false;
  let timer: ReturnType<typeof setTimeout> | undefined;
  async function poll() {
    again = true;
    if (listing) return;
    listing = true;
    clearTimeout(timer);
    while (again) {
      again = false;
      try {
        const res = await get('/api/backtests');
        if (!res.ok) {
          set({ ...state, offline: true });
          continue;
        }
        const runs = ((await res.json()) as { runs: RunListing[] }).runs;
        if (!state.strategies || !state.start) {
          const answer = await get('/api/strategies');
          if (answer.ok) set({ ...state, strategies: (await answer.json()) as Strategies });
          await loadDays();
        }
        set({ ...state, runs, offline: !state.strategies || !state.start });
        await fetchRun(state.selected);
      } catch {
        set({ ...state, offline: true });
      }
    }
    listing = false;
    const busy = state.runs.some((r) => r.state === 'queued' || r.state === 'running' || r.verifying !== null);
    if (busy || state.offline) timer = setTimeout(() => void poll(), 1_000);
  }

  // Queues a run and shows it; null, or why chronos-serve refused it.
  async function submit(body: RunSpecBody): Promise<LabError | null> {
    try {
      const res = await send('POST', '/api/backtests', body);
      const answer = (await res.json().catch(() => ({ error: String(res.status) }))) as LabError & { id: string };
      if (res.status !== 202) return answer;
      set({ ...state, selected: answer.id });
      await poll();
      return null;
    } catch {
      return { error: 'unreachable' };
    }
  }

  // Sends a change to a run, notes a refusal or a warning, and lists the runs again; true if it was done.
  async function act(what: string, method: string, path: string): Promise<boolean> {
    try {
      const res = await send(method, path);
      const answer = (await res.json().catch(() => ({}))) as { error?: string; warning?: string };
      set({ ...state, notice: res.ok ? (answer.warning ?? '') : `${what} refused: ${answer.error ?? res.status}` });
      return res.ok;
    } catch {
      set({ ...state, notice: `${what}: chronos-serve does not answer` });
      return false;
    } finally {
      await poll();
    }
  }

  return {
    get: () => state,
    subscribe(f: () => void) {
      listeners.add(f);
      return () => listeners.delete(f);
    },
    start: poll,
    loadDays,
    loadSymbols,
    submit,
    cancel: (id: string) => act('cancel', 'POST', `/api/backtests/${id}/cancel`),
    // Deletes a run: a busy one stops and is never saved.
    async remove(id: string) {
      if ((await act('delete', 'DELETE', `/api/backtests/${id}`)) && state.selected === id) {
        set({ ...state, selected: '' });
      }
    },
    // Shows run `id`; its JSON replaces the one kept.
    select(id: string) {
      set({ ...state, selected: id });
      return fetchRun(id);
    },
  };
}

export const lab = createLab({ fetch: (path, init) => fetch(path, init), load: (key) => sessionStorage.getItem(key) });

// A panel's slice of the Lab; selectors return stable references, as useStore's do.
export function useLab<T>(select: (s: LabState) => T): T {
  return useSyncExternalStore(lab.subscribe, () => select(lab.get()));
}
