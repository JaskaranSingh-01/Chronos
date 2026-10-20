import { useSyncExternalStore } from 'react';
import { dayTs, money, px, signed, tone } from './format';
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
  compare: string[]; // the runs ticked to compare, at most two
  comparing: boolean; // the comparison is shown rather than the selected run
  details: Record<string, Run>; // the JSON of the runs shown or ticked, kept until their state or verifications change
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

const counts = new Intl.NumberFormat('en-US');

// The stats of a run's results, each with how it reads: signed money for a PnL, money, a count, or a percentage.
const stats: [name: string, of: (r: Results) => number, kind: 'pnl' | 'money' | 'count' | 'percent'][] = [
  ['Total PnL', (r) => r.pnl, 'pnl'],
  ['Fills', (r) => r.fills, 'count'],
  ['Fill rate', (r) => r.fill_rate * 100, 'percent'],
  ['Max drawdown', (r) => r.max_drawdown, 'money'],
  ['Fees / rebates', (r) => -r.fees, 'pnl'], // as their effect on PnL, so a rebate is positive
  ['Max |position|', (r) => r.max_abs_position, 'count'],
];

// A run's stat cards as [name, value, tone], money at `digits` decimals.
export const cards = (r: Results, digits: number) =>
  stats.map(([name, of, kind]): [string, string, string] => {
    const n = of(r);
    if (kind === 'pnl') return [name, signed(n, digits), tone(n)];
    if (kind === 'money') return [name, money(n, digits), ''];
    return [name, kind === 'count' ? counts.format(n) : `${n.toFixed(1)}%`, ''];
  });

// Peak-to-date minus PnL at each sample; the peak starts at 0, as the Portfolio's does.
export function drawdown(pnl: number[]): number[] {
  let peak = 0;
  return pnl.map((p) => (peak = Math.max(peak, p)) - p);
}

// Sample times (epoch seconds) as day index x 86,400 plus the New York second of the day, so that runs over the same
// days line up exactly, and others by day and time of day.
export function align(t: number[], days: string[]): number[] {
  const midnights = days.map((day) => dayTs(day, '00:00') / 1e9);
  let day = 0;
  return t.map((s) => {
    while (day + 1 < midnights.length && s >= midnights[day + 1]) day++;
    return day * 86_400 + s - midnights[day];
  });
}

// An aligned time as the chart shows it: New York time, after the day's number when the runs span days.
export function timeOfRun(t: number, days: number): string {
  const s = t % 86_400;
  const hms = [s / 3_600, (s / 60) % 60, s % 60].map((n) => String(Math.floor(n)).padStart(2, '0')).join(':');
  return days > 1 ? `day ${Math.floor(t / 86_400) + 1} ${hms}` : hms;
}

const deltas = new Intl.NumberFormat('en-US', { signDisplay: 'exceptZero' });

// Two runs' stats side by side, with the second's difference from the first.
export function compareStats(a: Results, b: Results, digits: number): [string, string, string, string][] {
  const [left, right] = [cards(a, digits), cards(b, digits)];
  return stats.map(([name, of, kind], i) => {
    const d = of(b) - of(a);
    const delta =
      kind === 'pnl' || kind === 'money'
        ? signed(d, digits)
        : kind === 'count'
          ? deltas.format(d)
          : `${deltas.format(Number(d.toFixed(1)))} pp`;
    return [name, left[i][1], right[i][1], delta];
  });
}

// A run's settings as [field, value], each field named as in a RunSpec, fees in dollars.
export const specRows = (spec: Spec, digits: number): [string, string][] => [
  ['symbol', spec.symbol],
  ['days', spec.days.join(', ')],
  ['strategy', strategyNames[spec.strategy] ?? spec.strategy],
  ...Object.entries(spec.params).map(([name, value]): [string, string] => [`params.${name}`, String(value)]),
  ['window', windowLabels[spec.window]],
  ['latency.order_ns', String(spec.latency.order_ns)],
  ['latency.report_ns', String(spec.latency.report_ns)],
  ['fees.maker', px(spec.fees.maker, digits)],
  ['fees.taker', px(spec.fees.taker, digits)],
  ...Object.entries(spec.limits).map(([name, value]): [string, string] => [`limits.${name}`, String(value)]),
];

// Two runs' settings side by side: every field of either, a dash where one has none, and whether they differ.
export function specDiff(a: Spec, b: Spec, digits: number) {
  const [left, right] = [new Map(specRows(a, digits)), new Map(specRows(b, digits))];
  return [...new Set([...left.keys(), ...right.keys()])].map((field) => {
    const [x, y] = [left.get(field) ?? '—', right.get(field) ?? '—'];
    return { field, a: x, b: y, differs: x !== y };
  });
}

// Whether two runs replayed the same symbol over the same days.
export const likeForLike = (a: Spec, b: Spec) => a.symbol === b.symbol && a.days.join() === b.days.join();

// What a listed run allows: Cancel while it is queued or running; Watch and Verify once it completed whole, and Verify
// one at a time.
export function actions(r: RunListing) {
  const whole = r.state === 'completed' && !r.partial;
  return { cancel: r.state === 'queued' || r.state === 'running', watch: whole, verify: whole && r.verifying === null };
}

// A verdict as shown, with its colour: different code and a changed input prove nothing either way.
export const outcomes: Record<Outcome, [label: string, tone: string]> = {
  reproduced: ['✓ reproduced', 'buy'],
  mismatch: ['✗ mismatch', 'sell'],
  different_code: ['different code', 'muted'],
  input_changed: ['input changed', 'muted'],
};

// A run as the list numbers it, or its id when the list does not have it.
export function runLabel(runs: RunListing[], id: string): string {
  const i = runs.findIndex((r) => r.id === id);
  return i >= 0 ? `#${i + 1}` : id;
}

export function createLab(env: Pick<Env, 'fetch' | 'load'>) {
  let state: LabState = {
    strategies: null,
    start: '',
    days: [],
    symbols: [],
    runs: [],
    offline: false,
    selected: '',
    compare: [],
    comparing: false,
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

  // A shown or ticked run's JSON, unless the copy kept is of the state and the verifications listed.
  async function fetchRun(id: string) {
    const listed = state.runs.find((r) => r.id === id);
    const kept = state.details[id];
    if (!listed || (kept?.state === listed.state && kept.verifications.length === listed.verifications.length)) return;
    try {
      const res = await get(`/api/backtests/${id}`);
      const run = res.ok ? ((await res.json()) as Run) : null;
      if (run && (id === state.selected || state.compare.includes(id))) {
        set({ ...state, details: { ...state.details, [id]: run } });
      }
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
        const compare = state.compare.filter((id) => runs.some((r) => r.id === id)); // a deleted run drops out
        set({ ...state, runs, compare, offline: !state.strategies || !state.start });
        for (const id of [state.selected, ...state.compare]) await fetchRun(id);
      } catch {
        set({ ...state, offline: true });
      }
    }
    listing = false;
    const busy = state.runs.some((r) => r.state === 'queued' || r.state === 'running' || r.verifying !== null);
    if (busy || state.offline) timer = setTimeout(() => void poll(), 1_000);
  }

  // Shows `selected` and ticks `compare`, drops the JSON of any other run, and fetches theirs.
  async function view(selected: string, compare: string[], comparing: boolean) {
    const wanted = [selected, ...compare];
    const details = Object.fromEntries(Object.entries(state.details).filter(([id]) => wanted.includes(id)));
    set({ ...state, selected, compare, comparing, details });
    for (const id of wanted) await fetchRun(id);
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
    verify: (id: string) => act('verify', 'POST', `/api/backtests/${id}/verify`),
    // The run's JSON as chronos-serve serves it, for Export; null, said on the notice line, if it cannot be fetched.
    async download(id: string): Promise<Blob | null> {
      try {
        const res = await get(`/api/backtests/${id}`);
        if (res.ok) return await res.blob();
        set({ ...state, notice: `export refused: ${res.status}` });
      } catch {
        set({ ...state, notice: 'export: chronos-serve does not answer' });
      }
      return null;
    },
    // Deletes a run: a busy one stops and is never saved.
    async remove(id: string) {
      if ((await act('delete', 'DELETE', `/api/backtests/${id}`)) && state.selected === id) {
        set({ ...state, selected: '' });
      }
    },
    select: (id: string) => view(id, state.compare, false),
    // Ticks a run to compare, or unticks it; a third tick drops the first.
    tick(id: string, on: boolean) {
      const others = state.compare.filter((c) => c !== id);
      return view(state.selected, on ? [...others, id].slice(-2) : others, state.comparing);
    },
    showCompare: (comparing: boolean) => set({ ...state, comparing }),
  };
}

export const lab = createLab({ fetch: (path, init) => fetch(path, init), load: (key) => sessionStorage.getItem(key) });

// A panel's slice of the Lab; selectors return stable references, as useStore's do.
export function useLab<T>(select: (s: LabState) => T): T {
  return useSyncExternalStore(lab.subscribe, () => select(lab.get()));
}
