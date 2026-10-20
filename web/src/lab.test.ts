import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  actions,
  align,
  cards,
  compareStats,
  createLab,
  defaults,
  drawdown,
  fromSpec,
  likeForLike,
  page,
  refusalText,
  specDiff,
  timeOfRun,
  toBody,
  type Form,
  type Run,
  type RunListing,
  type Results,
  type RunSpecBody,
  type Strategies,
  type Verification,
} from './lab';
import type { Day, Spec } from './protocol';

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

const strategies: Strategies = {
  strategies: [
    {
      kind: 'market_maker',
      params: [
        { name: 'quote_size', default: 100, min: 1, max: 1_000_000_000 },
        { name: 'max_position', default: 500, min: 0, max: 1_000_000_000 },
        { name: 'requote_cooldown_ns', default: 100_000_000, min: 0, max: 1e15 },
        { name: 'improve_ticks', default: 0, min: 0, max: 1_000_000_000 },
      ],
      limits: { max_position: 500, max_order_quantity: 100, max_open_orders: 2 },
    },
    {
      kind: 'imbalance',
      params: [
        { name: 'threshold', default: 3, min: 1, max: null },
        { name: 'clip', default: 100, min: 1, max: 1_000_000_000 },
        { name: 'max_position', default: 500, min: 0, max: 1_000_000_000 },
        { name: 'cooldown_ns', default: 1_000_000_000, min: 0, max: 1e15 },
      ],
      limits: { max_position: 500, max_order_quantity: 100, max_open_orders: 1 },
    },
  ],
  latency: { order_ns: 50_000, report_ns: 50_000, max: 1e12 },
  fees: { maker: 0, taker: 0, min: -1_000_000, max: 1_000_000, taker_min: 0 },
  limit_max: 1_000_000_000,
  windows: [
    { id: 'regular', label: 'Regular (feed session)' },
    { id: 'full_day', label: 'Full day' },
  ],
  max_days: 32,
  max_queued: 8,
};

describe('page', () => {
  it('shows the Lab at #lab and the terminal at any other hash', () => {
    expect(page('#lab')).toBe('lab');
    for (const hash of ['', '#', '#labs', '#lab/1']) expect(page(hash)).toBe('terminal');
  });
});

describe('the run form', () => {
  const form = (over: Partial<Form> = {}): Form => ({
    ...defaults(strategies, 'market_maker', 4, ['2019-12-27', '2019-12-30']),
    symbol: 'acme',
    ...over,
  });

  it('starts at the defaults /api/strategies serves, the fees in dollars and the limits empty to follow the params', () => {
    expect(defaults(strategies, 'imbalance', 4, ['2019-12-30'])).toEqual({
      strategy: 'imbalance',
      symbol: '',
      days: ['2019-12-30'],
      window: 'regular',
      params: { threshold: '3', clip: '100', max_position: '500', cooldown_ns: '1000000000' },
      latency: { order_ns: '50000', report_ns: '50000' },
      fees: { maker: '0.00', taker: '0.00' },
      limits: { max_position: '', max_order_quantity: '', max_open_orders: '' },
    });
  });

  it('refuses a value past the cap /api/strategies serves, naming the field as chronos-serve does', () => {
    const capped = { ...strategies, latency: { ...strategies.latency, max: 1_000 } };
    expect(toBody(form({ latency: { order_ns: '1000', report_ns: '0' } }), capped, 4)).toHaveProperty('latency');
    expect(toBody(form({ latency: { order_ns: '1001', report_ns: '0' } }), capped, 4)).toBe('latency.order_ns');
    const f = form();
    const cases: [Partial<Form>, string][] = [
      [{ params: { ...f.params, quote_size: '0' } }, 'params.quote_size'],
      [{ params: { ...f.params, improve_ticks: '' } }, 'params.improve_ticks'],
      [{ symbol: ' ' }, 'symbol'],
      [{ days: [] }, 'days'],
      [{ latency: { order_ns: '50000', report_ns: '-1' } }, 'latency.report_ns'],
      [{ fees: { maker: '-100.0001', taker: '0' } }, 'fees.maker'], // -1,000,001 ticks
      [{ fees: { maker: '0.00001', taker: '0' } }, 'fees.maker'], // finer than a tick
      [{ fees: { maker: '0', taker: '-0.0001' } }, 'fees.taker'], // a taker fee is never a rebate
      [{ limits: { ...f.limits, max_open_orders: '1.5' } }, 'limits.max_open_orders'],
      [{ limits: { ...f.limits, max_position: '1000000001' } }, 'limits.max_position'],
    ];
    for (const [over, field] of cases) expect(toBody(form(over), strategies, 4)).toBe(field);
  });

  it('posts the fees in ticks and only the limits typed, and fills the form back from a run for Edit as new', () => {
    const typed = form({
      window: 'full_day',
      fees: { maker: '-0.002', taker: '0.003' },
      limits: { max_position: '', max_order_quantity: '200', max_open_orders: '' },
    });
    expect(toBody(typed, strategies, 4)).toEqual({
      symbol: 'ACME',
      days: ['2019-12-27', '2019-12-30'],
      strategy: 'market_maker',
      params: { quote_size: 100, max_position: 500, requote_cooldown_ns: 100_000_000, improve_ticks: 0 },
      latency: { order_ns: 50_000, report_ns: 50_000 },
      fees: { maker: -20, taker: 30 },
      limits: { max_order_quantity: 200 },
      window: 'full_day',
    });
    expect(fromSpec({ ...spec, fees: { maker: -20, taker: 30 } }, 4)).toEqual({
      strategy: 'market_maker',
      symbol: 'ACME',
      days: ['2019-12-30'],
      window: 'regular',
      params: { quote_size: '100', max_position: '500', requote_cooldown_ns: '100000000', improve_ticks: '0' },
      latency: { order_ns: '50000', report_ns: '50000' },
      fees: { maker: '-0.0020', taker: '0.0030' },
      limits: { max_position: '500', max_order_quantity: '100', max_open_orders: '2' },
    });
  });

  it("says why chronos-serve refused a run: a missing symbol's days, the field it named, or the status", () => {
    expect(refusalText({ error: 'symbol_missing', days: ['2019-12-27'] }, 'ZETA')).toBe(
      'ZETA is not listed on 2019-12-27',
    );
    expect(refusalText({ error: 'bad_request', field: 'days[1]' }, 'ACME')).toBe('Check days[1]');
    expect(refusalText({ error: 'bad_request' }, 'ACME')).toBe('Refused: bad_request');
    expect(refusalText({ error: 'queue_full' }, 'ACME')).toBe('Refused: the queue is full');
    expect(refusalText({ error: '403' }, 'ACME')).toBe('Refused: 403');
  });

  it('allows Cancel only while a run is queued or running', () => {
    const states = ['queued', 'running', 'completed', 'failed', 'cancelled'] as const;
    expect(states.map((state) => actions(listed('a', { state })).cancel)).toEqual([true, true, false, false, false]);
  });
});

describe('results', () => {
  it('shows the stat cards: PnL and fees signed and coloured, money in dollars, the fill rate in percent', () => {
    const r = {
      pnl: -2_004_400,
      fills: 12_843,
      fill_rate: 0.1974,
      max_drawdown: 5_155_600,
      fees: -21_117_100,
      max_abs_position: 500,
    } as Results;
    expect(cards(r, 4)).toEqual([
      ['Total PnL', '-$200.44', 'sell'],
      ['Fills', '12,843', ''],
      ['Fill rate', '19.7%', ''],
      ['Max drawdown', '$515.56', ''],
      ['Fees / rebates', '+$2,111.71', 'buy'],
      ['Max |position|', '500', ''],
    ]);
  });

  it('draws the drawdown as the peak so far, from 0, minus the PnL', () => {
    expect(drawdown([-5, 10, 4, 12, 12, 7])).toEqual([5, 0, 6, 0, 0, 5]);
  });

  it('lines samples up by day index and New York time of day, across a daylight-saving change, and labels them', () => {
    const at = (iso: string) => Date.parse(iso) / 1_000;
    expect(align([at('2019-12-27T14:30:01Z'), at('2019-12-30T14:30:02Z')], ['2019-12-27', '2019-12-30'])).toEqual([
      34_201,
      86_400 + 34_202,
    ]);
    // Friday in standard time, Monday in daylight time: 09:30 either way.
    expect(align([at('2019-03-08T14:30:00Z'), at('2019-03-11T13:30:00Z')], ['2019-03-08', '2019-03-11'])).toEqual([
      34_200,
      86_400 + 34_200,
    ]);
    expect([timeOfRun(34_201, 1), timeOfRun(86_400 + 57_599, 2)]).toEqual(['09:30:01', 'day 2 15:59:59']);
  });
});

describe('compare', () => {
  const results = (over: Partial<Results>) =>
    ({ pnl: 0, fills: 0, fill_rate: 0, max_drawdown: 0, fees: 0, max_abs_position: 0, ...over }) as Results;

  it("shows two runs' stats side by side, with the second's difference from the first", () => {
    const a = results({
      pnl: 30_000,
      fills: 6,
      fill_rate: 0.3529,
      max_drawdown: 10_000,
      fees: 0,
      max_abs_position: 100,
    });
    const b = results({ pnl: 50_000, fills: 10, fill_rate: 0.3448, max_drawdown: 10_000, fees: -2_000 });
    expect(compareStats(a, b, 4)).toEqual([
      ['Total PnL', '+$3.00', '+$5.00', '+$2.00'],
      ['Fills', '6', '10', '+4'],
      ['Fill rate', '35.3%', '34.5%', '-0.8 pp'],
      ['Max drawdown', '$1.00', '$1.00', '$0.00'],
      ['Fees / rebates', '$0.00', '+$0.20', '+$0.20'],
      ['Max |position|', '100', '0', '-100'],
    ]);
  });

  it('marks every setting that differs, and runs over another symbol or other days as not like-for-like', () => {
    const other: Spec = {
      ...spec,
      days: ['2019-12-27', '2019-12-30'],
      strategy: 'imbalance',
      params: { threshold: 3, clip: 100, max_position: 500, cooldown_ns: 1_000_000_000 },
      fees: { maker: -20, taker: 30 },
      window: 'full_day',
    };
    const diff = specDiff(spec, other, 4);
    expect(diff.filter((row) => row.differs).map((row) => [row.field, row.a, row.b])).toEqual([
      ['days', '2019-12-30', '2019-12-27, 2019-12-30'],
      ['strategy', 'Market maker', 'Imbalance'],
      ['params.quote_size', '100', '—'],
      ['params.requote_cooldown_ns', '100000000', '—'],
      ['params.improve_ticks', '0', '—'],
      ['window', 'Regular (feed session)', 'Full day'],
      ['fees.maker', '0.00', '-0.0020'],
      ['fees.taker', '0.00', '0.0030'],
      ['params.threshold', '—', '3'],
      ['params.clip', '—', '100'],
      ['params.cooldown_ns', '—', '1000000000'],
    ]);
    expect(diff.find((row) => row.field === 'params.max_position')).toEqual({
      field: 'params.max_position',
      a: '500',
      b: '500',
      differs: false,
    });
    expect([likeForLike(spec, { ...spec, window: 'full_day' }), likeForLike(spec, other)]).toEqual([true, false]);
    expect(likeForLike(spec, { ...spec, symbol: 'ZETA' })).toBe(false);
  });
});

describe('createLab', () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const day = (date: string): Day => ({
    date,
    file: '',
    size: 0,
    hash: '',
    quick_check: '',
    itch50: true,
    cut_tail: false,
    session: { open: 0, close: 0, source: 'feed' },
    symbols: 2,
  });

  // chronos-serve's Lab: the runs it lists and each run's JSON, `answer` for a change; `down` fails every request.
  function server() {
    const s = {
      runs: [] as RunListing[],
      json: {} as Record<string, Run>,
      down: false,
      token: '',
      fetched: [] as string[], // 'GET /path Bearer t'
      sent: [] as { type?: string; body: unknown }[], // each change's Content-Type and body
      answer: (_method: string, _path: string): Response | undefined => undefined,
    };
    const env = {
      async fetch(path: string, init?: RequestInit) {
        const headers = (init?.headers ?? {}) as Record<string, string>;
        const method = init?.method ?? 'GET';
        s.fetched.push(`${method} ${path}${headers.Authorization ? ` ${headers.Authorization}` : ''}`);
        if (s.down) throw new TypeError('Failed to fetch');
        if (method !== 'GET') s.sent.push({ type: headers['Content-Type'], body: JSON.parse(String(init?.body)) });
        const answer = s.answer(method, path);
        if (answer) return answer;
        if (path === '/api/strategies') return Response.json(strategies);
        if (path === '/api/days')
          return Response.json({ start: '2019-12-30', days: [day('2019-12-27'), day('2019-12-30')] });
        if (path.startsWith('/api/symbols?days=')) return Response.json({ symbols: ['ACME'] });
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

  it('loads the strategies and the days once, the days again when asked, and the symbols on every day picked', async () => {
    const { s, env } = server();
    const lab = createLab(env);
    await lab.start();
    await lab.start();
    expect([lab.get().strategies, lab.get().start, lab.get().days.length]).toEqual([strategies, '2019-12-30', 2]);
    await lab.loadDays();
    await lab.loadSymbols(['2019-12-27', '2019-12-30']);
    expect(lab.get().symbols).toEqual(['ACME']);
    expect(s.fetched.filter((f) => !f.startsWith('GET /api/backtests'))).toEqual([
      'GET /api/strategies',
      'GET /api/days',
      'GET /api/days',
      'GET /api/symbols?days=2019-12-27,2019-12-30',
    ]);
  });

  it('posts a run as JSON with the token and shows it, or answers why chronos-serve refused it', async () => {
    const { s, env } = server();
    s.token = 't0k3n';
    s.answer = (method, path) => {
      if (method !== 'POST' || path !== '/api/backtests') return undefined;
      if ((s.sent.at(-1)?.body as { symbol: string }).symbol === 'ZETA') {
        return Response.json({ error: 'symbol_missing', days: ['2019-12-27'] }, { status: 400 });
      }
      s.runs.push(listed('n', { state: 'queued', progress: 0, pnl: null, fills: null }));
      return Response.json({ id: 'n' }, { status: 202 });
    };
    const lab = createLab(env);
    const acme: RunSpecBody = { symbol: 'ACME', days: ['2019-12-30'], strategy: 'market_maker' };
    expect(await lab.submit({ ...acme, symbol: 'ZETA', days: ['2019-12-27', '2019-12-30'] })).toEqual({
      error: 'symbol_missing',
      days: ['2019-12-27'],
    });
    expect(await lab.submit(acme)).toBeNull();
    expect([lab.get().selected, lab.get().runs.map((r) => r.state)]).toEqual(['n', ['queued']]);
    expect(s.fetched.filter((f) => f.startsWith('POST'))).toEqual([
      'POST /api/backtests Bearer t0k3n',
      'POST /api/backtests Bearer t0k3n',
    ]);
    expect(s.sent[1]).toEqual({
      type: 'application/json',
      body: { symbol: 'ACME', days: ['2019-12-30'], strategy: 'market_maker' },
    });
    s.down = true;
    expect(await lab.submit(acme)).toEqual({ error: 'unreachable' });
  });

  it('cancels and deletes a run as JSON, notes a refusal or a warning, and lists the runs again', async () => {
    const { s, env } = server();
    s.runs = [listed('a', { state: 'running', progress: 0.5 }), listed('b')];
    s.answer = (method, path) => {
      if (path === '/api/backtests/a/cancel') {
        s.runs[0] = listed('a', { state: 'cancelled', partial: true });
        return new Response(null, { status: 204 });
      }
      if (path === '/api/backtests/b/cancel') return new Response(null, { status: 409 });
      if (method === 'DELETE' && path === '/api/backtests/b') {
        s.runs.pop();
        return Response.json({ warning: 'not removed: Permission denied' });
      }
      return undefined;
    };
    const lab = createLab(env);
    await lab.start();
    await lab.select('b');
    await lab.cancel('a');
    expect([lab.get().runs[0].state, lab.get().notice]).toEqual(['cancelled', '']);
    await lab.cancel('b');
    expect(lab.get().notice).toBe('cancel refused: 409');
    await lab.remove('b');
    expect([lab.get().notice, lab.get().selected, lab.get().runs.length]).toEqual([
      'not removed: Permission denied',
      '',
      1,
    ]);
    expect(s.sent.map((c) => c.type)).toEqual(['application/json', 'application/json', 'application/json']);
  });

  it('keeps the JSON of the shown run and the two ticked to compare; a third tick drops the first, a deleted run its own', async () => {
    const { s, env } = server();
    s.runs = ['a', 'b', 'c'].map((id) => listed(id));
    s.json = { a: run('a'), b: run('b'), c: run('c') };
    const lab = createLab(env);
    await lab.start();
    await lab.select('a');
    await lab.tick('b', true);
    await lab.tick('c', true);
    expect([lab.get().compare, Object.keys(lab.get().details).sort()]).toEqual([
      ['b', 'c'],
      ['a', 'b', 'c'],
    ]);
    await lab.tick('a', true); // a third: b, the first, goes
    expect([lab.get().compare, Object.keys(lab.get().details).sort()]).toEqual([
      ['c', 'a'],
      ['a', 'c'],
    ]);
    s.runs = s.runs.filter((r) => r.id !== 'c'); // deleted, as from another tab
    await lab.start();
    lab.showCompare(true);
    await lab.select('b');
    expect([lab.get().compare, lab.get().comparing, Object.keys(lab.get().details).sort()]).toEqual([
      ['a'],
      false,
      ['a', 'b'],
    ]);
    expect(s.fetched.filter((f) => f.startsWith('GET /api/backtests/'))).toEqual([
      'GET /api/backtests/a',
      'GET /api/backtests/b',
      'GET /api/backtests/c',
      'GET /api/backtests/b',
    ]);
  });
});
