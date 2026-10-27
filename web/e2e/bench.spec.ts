import { type APIRequestContext, expect, test } from '@playwright/test';
import { mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { serve } from './serve';

// Configuration 3: chronos-serve at max speed with this page attached and trading. With CHRONOS_BENCH_LAB,
// SYMBOL:strategy, configuration 4: that Lab run over the same day is in flight as the page plays, and
// then runs again on the idle server. CHRONOS_BENCH_FILE names an ITCH day (a cut slice works); the summary is printed,
// and written to CHRONOS_BENCH_OUT if set. CHRONOS_BENCH_STORE names a store that keeps the day once built; without it,
// each run builds the day into a temp store, which needs about a third of the day's size free.
const file = process.env.CHRONOS_BENCH_FILE;
const store = process.env.CHRONOS_BENCH_STORE ? ['--store', process.env.CHRONOS_BENCH_STORE] : [];
const date = process.env.CHRONOS_BENCH_DATE ?? '2019-12-30';
const symbol = process.env.CHRONOS_BENCH_SYMBOL ?? 'AAPL';
const lab = process.env.CHRONOS_BENCH_LAB;

// Queues the Lab run as the Lab page does: JSON, from the page's origin.
async function postRun(request: APIRequestContext, url: string): Promise<string> {
  const [labSymbol, strategy] = lab!.split(':');
  const response = await request.post(`${url}api/backtests`, {
    data: { symbol: labSymbol, days: [date], strategy, window: 'full_day' },
    headers: { Origin: url.slice(0, -1) },
  });
  expect(response.status()).toBe(202);
  return (await response.json()).id;
}

// The run's wall time in ms, once it has finished.
async function wallMs(request: APIRequestContext, url: string, id: string): Promise<number> {
  for (;;) {
    const run = await (await request.get(`${url}api/backtests/${id}`)).json();
    if (run.state !== 'queued' && run.state !== 'running') return run.provenance.wall_ms;
    await new Promise((done) => setTimeout(done, 500));
  }
}

test.skip(!file, 'set CHRONOS_BENCH_FILE to run the interactive benchmark');

test('interactive benchmark', async ({ page }) => {
  test.setTimeout(10 * 60_000);
  const data = mkdtempSync(join(tmpdir(), 'chronos-bench-'));
  let server: Awaited<ReturnType<typeof serve>> | undefined;
  try {
    const [y, m, d] = date.split('-');
    symlinkSync(resolve(file!), join(data, `${m}${d}${y}.NASDAQ_ITCH50.bin`));
    server = await serve(['--data', data, '--date', date, '--allow-truncated-tail', ...store]);
    const seen: { playing: boolean; eps: number; dropped: number; queue: number; events: number }[] = [];
    page.on('websocket', (ws) =>
      ws.on('framereceived', ({ payload }) => {
        const s = JSON.parse(String(payload));
        if (s.type !== 'snapshot' && s.type !== 'state') return;
        const { events_per_second: eps, dropped_snapshots: dropped, command_queue_depth: queue } = s.metrics;
        seen.push({ playing: s.state === 'playing', eps, dropped, queue, events: s.event_seq });
      }),
    );
    await page.goto(server.url);
    await page.getByLabel('Symbol').fill(symbol);
    await page.getByLabel('Symbol').press('Enter');
    await expect(page.getByTestId('session')).toContainText(`${symbol} · gen 2 · paused`, { timeout: 60_000 });
    await page.getByRole('button', { name: 'Max', exact: true }).click();
    await page.getByRole('button', { name: 'Market', exact: true }).click();
    await page.getByLabel('Qty').fill('1');
    await page.evaluate(() => {
      const w = window as unknown as { fps: { frames: number; worst: number; first: number; last: number } };
      w.fps = { frames: 0, worst: 0, first: performance.now(), last: performance.now() };
      const frame = (t: number) => {
        w.fps.frames++;
        w.fps.worst = Math.max(w.fps.worst, t - w.fps.last);
        w.fps.last = t;
        requestAnimationFrame(frame);
      };
      requestAnimationFrame(frame);
    });
    const start = Date.now();
    const busyRun = lab ? await postRun(page.request, server.url) : undefined;
    await page.getByRole('button', { name: '▶ Play' }).click();
    // Trade by hand while it replays: alternate 1-share market buys and sells, as fast as the ticket takes them.
    let orders = 0;
    while (!(await page.getByTestId('session').textContent())?.includes('· ended')) {
      await page.getByRole('button', { name: orders % 2 === 0 ? 'Buy' : 'Sell', exact: true }).click();
      if (await page.locator('.place').isEnabled()) {
        await page.locator('.place').click();
        orders++;
      }
      await page.waitForTimeout(100);
    }
    const seconds = (Date.now() - start) / 1_000;
    const ui = await page.evaluate(
      () => (window as unknown as { fps: { frames: number; worst: number; first: number; last: number } }).fps,
    );

    const labRun = lab && {
      run: lab,
      wall_ms_while_playing: await wallMs(page.request, server.url, busyRun!),
      wall_ms_idle: await wallMs(page.request, server.url, await postRun(page.request, server.url)),
    };

    const playing = seen.filter((s) => s.playing);
    const eps = playing.map((s) => s.eps).sort((a, b) => a - b);
    const summary = {
      file,
      symbol,
      wall_seconds: Number(seconds.toFixed(1)),
      market_events: seen.at(-1)?.events ?? 0,
      events_per_second: {
        mean: Math.round((seen.at(-1)?.events ?? 0) / seconds),
        snapshot_median: eps[Math.floor(eps.length / 2)] ?? 0,
        snapshot_max: eps.at(-1) ?? 0,
      },
      snapshots_per_second: Number((playing.length / seconds).toFixed(1)),
      dropped_snapshots: seen.at(-1)?.dropped ?? 0,
      command_queue_depth_max: Math.max(0, ...seen.map((s) => s.queue)),
      orders_sent: orders,
      ui_frames_per_second: Number(((1_000 * ui.frames) / (ui.last - ui.first)).toFixed(1)),
      ui_worst_frame_ms: Math.round(ui.worst),
      lab: labRun,
    };
    console.log(JSON.stringify(summary, null, 2));
    if (process.env.CHRONOS_BENCH_OUT)
      writeFileSync(process.env.CHRONOS_BENCH_OUT, `${JSON.stringify(summary, null, 2)}\n`);
  } finally {
    try {
      await server?.stop();
    } finally {
      rmSync(data, { recursive: true, force: true });
    }
  }
});
