import { useSyncExternalStore } from 'react';
import type { Digests, FillRule, Outcome, Spec } from './protocol';
import { tokenKey, type Env } from './store';

// chronos-serve's Lab over HTTP (server/src/protocol.cpp): money in tick-shares, times in epoch ns but equity.t.

export type RunState = 'queued' | 'running' | 'completed' | 'failed' | 'cancelled';

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
  runs: RunListing[]; // oldest first: run #1 is the first
  offline: boolean; // the last listing failed; polling goes on until one answers
  selected: string; // the run shown; '' for none
  details: Record<string, Run>; // the shown run's JSON, kept until its state or its verifications change
}

export const strategyNames: Record<string, string> = { market_maker: 'Market maker', imbalance: 'Imbalance' };

// The page a location hash shows: the Lab at #lab, else the terminal.
export const page = (hash: string) => (hash === '#lab' ? 'lab' : 'terminal');

// A run's money decimals: tick-shares are 10^(price_scale + quantity_scale) a dollar.
export const moneyDigits = (r: Pick<Run, 'provenance'>) =>
  r.provenance.engine.price_scale + r.provenance.engine.quantity_scale;

export function createLab(env: Pick<Env, 'fetch' | 'load'>) {
  let state: LabState = { runs: [], offline: false, selected: '', details: {} };
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
        set({ ...state, runs: ((await res.json()) as { runs: RunListing[] }).runs, offline: false });
        await fetchRun(state.selected);
      } catch {
        set({ ...state, offline: true });
      }
    }
    listing = false;
    const busy = state.runs.some((r) => r.state === 'queued' || r.state === 'running' || r.verifying !== null);
    if (busy || state.offline) timer = setTimeout(() => void poll(), 1_000);
  }

  return {
    get: () => state,
    subscribe(f: () => void) {
      listeners.add(f);
      return () => listeners.delete(f);
    },
    start: poll,
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
