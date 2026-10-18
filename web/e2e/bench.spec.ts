import { expect, test } from '@playwright/test';
import { mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { serve } from './serve';

// Spec §9, configuration 3: chronos-serve at max speed with this page attached and trading. CHRONOS_BENCH_FILE names
// an ITCH day (a cut slice works); the summary is printed, and written to CHRONOS_BENCH_OUT if set.
const file = process.env.CHRONOS_BENCH_FILE;
const date = process.env.CHRONOS_BENCH_DATE ?? '2019-12-30';
const symbol = process.env.CHRONOS_BENCH_SYMBOL ?? 'AAPL';

test.skip(!file, 'set CHRONOS_BENCH_FILE to run the interactive benchmark');

test('interactive benchmark', async ({ page }) => {
  test.setTimeout(10 * 60_000);
  const data = mkdtempSync(join(tmpdir(), 'chronos-bench-'));
  const [y, m, d] = date.split('-');
  symlinkSync(resolve(file!), join(data, `${m}${d}${y}.NASDAQ_ITCH50.bin`));
  const server = await serve(['--data', data, '--date', date, '--allow-truncated-tail']);
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
  await server.stop();
  rmSync(data, { recursive: true });

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
  };
  console.log(JSON.stringify(summary, null, 2));
  if (process.env.CHRONOS_BENCH_OUT)
    writeFileSync(process.env.CHRONOS_BENCH_OUT, `${JSON.stringify(summary, null, 2)}\n`);
});
