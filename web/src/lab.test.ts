import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createLab, page, type Run, type RunListing, type Verification } from './lab';
import type { Spec } from './protocol';

const spec: Spec = {
  symbol: 'ACME',
  days: ['2019-12-30'],
  strategy: 'market_maker',
  params: { quote_size: 100, max_position: 500, requote_cooldown_ns: 100_000_000, improve_ticks: 0 },
  latency: { order_ns: 50_000, report_ns: 50_000 },
  fees: { maker: 0, taker: 0 },
  limits: { max_position: 500, max_order_quantity: 100, max_open_orders: 2 },
  window: 'regular',
};

const base = (id: string): Omit<Run, 'results'> => ({
  format: 1,
  id,
  state: 'completed',
  reason: '',
  warning: '',
  spec,
  provenance: {
    build: {
      commit: '51d88f8',
      dirty: false,
      source_digest: 'a'.repeat(64),
      compiler: 'AppleClang 17.0.0',
      build_type: 'RelWithDebInfo',
      sanitize: 'OFF',
      platform: 'Darwin arm64',
    },
    engine: { book_capacity: 1_048_576, price_scale: 4, quantity_scale: 0, check_invariants_every: 0 },
    allow_truncated_tail: false,
    days: [],
    host: 'h',
    submitted: '2026-10-20T06:00:00Z',
    started: '2026-10-20T06:00:00Z',
    finished: '2026-10-20T06:00:01Z',
    wall_ms: 20,
    cpu_ms: 15,
  },
  verifying: null,
  verifications: [],
  reproduced: null,
});
const run = (id: string, over: Partial<Run> = {}): Run => ({ ...base(id), results: null, ...over });
const listed = (id: string, over: Partial<RunListing> = {}): RunListing => ({
  ...base(id),
  progress: 1,
  partial: false,
  pnl: 30_000,
  fills: 6,
  ...over,
});

describe('page', () => {
  it('shows the Lab at #lab and the terminal at any other hash', () => {
    expect(page('#lab')).toBe('lab');
    for (const hash of ['', '#', '#labs', '#lab/1']) expect(page(hash)).toBe('terminal');
  });
});

describe('createLab', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  // chronos-serve's Lab: the runs it lists and each run's JSON; `down` makes every request fail.
  function server() {
    const s = {
      runs: [] as RunListing[],
      json: {} as Record<string, Run>,
      down: false,
      token: '',
      fetched: [] as string[], // 'GET /path Bearer t'
    };
    const env = {
      async fetch(path: string, init?: RequestInit) {
        const auth = (init?.headers as Record<string, string> | undefined)?.Authorization;
        s.fetched.push(`${init?.method ?? 'GET'} ${path}${auth ? ` ${auth}` : ''}`);
        if (s.down) throw new TypeError('Failed to fetch');
        if (path === '/api/backtests') return Response.json({ runs: s.runs, queued: 0, max_queued: 8 });
        const id = /^\/api\/backtests\/([^/]+)$/.exec(path)?.[1];
        return id && s.json[id] ? Response.json(s.json[id]) : new Response(null, { status: 404 });
      },
      load: (key: string) => (key === 'chronos-token' && s.token ? s.token : null),
    };
    return { s, env, listings: () => s.fetched.filter((f) => f.split(' ')[1] === '/api/backtests').length };
  }

  it('polls the runs each second while a run or a verification is queued or running, and then stops', async () => {
    const { s, env, listings } = server();
    s.runs = [listed('a', { state: 'running', progress: 0.25, pnl: null, fills: null })];
    const lab = createLab(env);
    await lab.start();
    expect([listings(), lab.get().runs[0].progress]).toEqual([1, 0.25]);
    await vi.advanceTimersByTimeAsync(1_000);
    expect(listings()).toBe(2);
    s.runs = [listed('a'), listed('b', { state: 'queued' })];
    await vi.advanceTimersByTimeAsync(1_000);
    s.runs = [listed('a', { verifying: 'running' }), listed('b')];
    await vi.advanceTimersByTimeAsync(1_000);
    expect(listings()).toBe(4);
    s.runs = [listed('a'), listed('b')]; // nothing active: this listing is the last
    await vi.advanceTimersByTimeAsync(1_000);
    await vi.advanceTimersByTimeAsync(10_000);
    expect(listings()).toBe(5);
    expect(lab.get().runs.map((r) => r.state)).toEqual(['completed', 'completed']);
  });

  it('keeps polling while chronos-serve does not answer, says so, and stops once it answers with nothing active', async () => {
    const { s, env, listings } = server();
    s.down = true;
    const lab = createLab(env);
    await lab.start();
    expect(lab.get().offline).toBe(true);
    await vi.advanceTimersByTimeAsync(3_000);
    expect(listings()).toBe(4);
    s.down = false;
    s.runs = [listed('a')];
    await vi.advanceTimersByTimeAsync(1_000);
    expect([lab.get().offline, lab.get().runs.length]).toEqual([false, 1]);
    await vi.advanceTimersByTimeAsync(10_000);
    expect(listings()).toBe(5);
  });

  it("fetches a shown run's JSON with the token once, and again only when its state or verifications change", async () => {
    const { s, env } = server();
    s.token = 't0k3n';
    s.runs = [listed('a', { state: 'running', progress: 0.5 }), listed('b')];
    s.json = { a: run('a', { state: 'running' }), b: run('b') };
    const lab = createLab(env);
    await lab.start();
    await lab.select('a');
    expect(lab.get().details.a.state).toBe('running');
    await vi.advanceTimersByTimeAsync(1_000); // still running: the JSON kept is current
    s.runs = [listed('a'), listed('b')];
    s.json.a = run('a');
    await vi.advanceTimersByTimeAsync(1_000); // completed: fetched again, with its results
    expect(lab.get().details.a.state).toBe('completed');
    await lab.select('a'); // unchanged: no fetch
    const verified: Verification = {
      finished: '',
      outcome: 'reproduced',
      digests: null,
      source_digest: '',
      changed: [],
    };
    s.runs = [listed('a', { verifications: [verified] }), listed('b')];
    s.json.a = run('a', { verifications: [verified] });
    await lab.start();
    expect(lab.get().details.a.verifications).toHaveLength(1);
    await lab.select('b'); // a's JSON is dropped once another run is shown
    expect(Object.keys(lab.get().details)).toEqual(['b']);
    expect(s.fetched.filter((f) => f.startsWith('GET /api/backtests/'))).toEqual([
      'GET /api/backtests/a Bearer t0k3n',
      'GET /api/backtests/a Bearer t0k3n',
      'GET /api/backtests/a Bearer t0k3n',
      'GET /api/backtests/b Bearer t0k3n',
    ]);
  });
});
