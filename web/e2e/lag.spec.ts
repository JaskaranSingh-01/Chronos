import { expect, test, type Page } from '@playwright/test';
import { mkdtempSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { serve } from './serve';

// Where the page's time goes in the three places it was seen to lag: playing a replay, switching to the Lab while one
// plays, and running then watching a strategy. CHRONOS_LAG_FILE names an ITCH day; CHRONOS_LAG_STORE, a store that
// keeps it. Each window's summary is printed, and written to CHRONOS_LAG_OUT if set. It measures; it asserts nothing.
const file = process.env.CHRONOS_LAG_FILE;
const store = process.env.CHRONOS_LAG_STORE ? ['--store', process.env.CHRONOS_LAG_STORE] : [];
const date = process.env.CHRONOS_LAG_DATE ?? '2019-12-30';
const symbol = process.env.CHRONOS_LAG_SYMBOL ?? 'AAPL';

interface Lag {
  start: number;
  messages: number;
  bytes: number;
  handlerMs: number;
  handlerMax: number;
  longTasks: number[];
  frames: number;
  last: number;
  worstFrame: number;
  slowFrames: number;
}

// Before any page script: times every socket message's handler, and records long tasks and frame gaps.
function instrument() {
  const w = window as unknown as { lag: Lag; resetLag(): void };
  w.resetLag = () => {
    const now = performance.now();
    w.lag = {
      start: now,
      messages: 0,
      bytes: 0,
      handlerMs: 0,
      handlerMax: 0,
      longTasks: [],
      frames: 0,
      last: now,
      worstFrame: 0,
      slowFrames: 0,
    };
  };
  w.resetLag();
  const onmessage = Object.getOwnPropertyDescriptor(WebSocket.prototype, 'onmessage')!;
  Object.defineProperty(WebSocket.prototype, 'onmessage', {
    set(handler: (e: MessageEvent) => void) {
      onmessage.set!.call(this, (e: MessageEvent) => {
        const t = performance.now();
        handler(e);
        const ms = performance.now() - t;
        w.lag.messages++;
        w.lag.bytes += String(e.data).length;
        w.lag.handlerMs += ms;
        w.lag.handlerMax = Math.max(w.lag.handlerMax, ms);
      });
    },
  });
  new PerformanceObserver((list) => list.getEntries().forEach((e) => w.lag.longTasks.push(e.duration))).observe({
    type: 'longtask',
  });
  const frame = (t: number) => {
    const gap = t - w.lag.last;
    w.lag.frames++;
    w.lag.worstFrame = Math.max(w.lag.worstFrame, gap);
    if (gap > 50) w.lag.slowFrames++;
    w.lag.last = t;
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

const windows: Record<string, unknown>[] = [];

// Measures `act` and then `settle` ms more as one window named `name`; the API fetches are listed from the resource
// timeline, by their transferred bytes.
async function measure(page: Page, name: string, act: () => Promise<unknown>, settle = 0) {
  await page.evaluate(() => {
    (window as unknown as { resetLag(): void }).resetLag();
    performance.clearResourceTimings();
  });
  await act();
  await page.waitForTimeout(settle);
  const summary = await page.evaluate((name) => {
    const lag = (window as unknown as { lag: Lag }).lag;
    const seconds = (performance.now() - lag.start) / 1_000;
    const api = performance
      .getEntriesByType('resource')
      .filter((e) => e.name.includes('/api/')) as PerformanceResourceTiming[];
    const round = (x: number) => Math.round(x * 10) / 10;
    return {
      window: name,
      seconds: round(seconds),
      ws_messages_per_second: round(lag.messages / seconds),
      ws_kb_per_second: round(lag.bytes / 1_024 / seconds),
      handler_ms_total: round(lag.handlerMs),
      handler_ms_max: round(lag.handlerMax),
      handler_share_percent: round(lag.handlerMs / 10 / seconds),
      long_tasks: lag.longTasks.length,
      long_task_ms_total: round(lag.longTasks.reduce((a, b) => a + b, 0)),
      long_task_ms_max: round(Math.max(0, ...lag.longTasks)),
      frames_per_second: round(lag.frames / seconds),
      worst_frame_ms: round(lag.worstFrame),
      frames_over_50ms: lag.slowFrames,
      api: api.map((e) => ({
        path: new URL(e.name).pathname,
        kb: round(e.transferSize / 1_024),
        ms: round(e.duration),
      })),
    };
  }, name);
  console.log(JSON.stringify(summary));
  windows.push(summary);
}

test.skip(!file, 'set CHRONOS_LAG_FILE to measure where the page lags');

test('UI lag', async ({ page }) => {
  test.setTimeout(15 * 60_000);
  const data = mkdtempSync(join(tmpdir(), 'chronos-lag-'));
  let server: Awaited<ReturnType<typeof serve>> | undefined;
  try {
    const [y, m, d] = date.split('-');
    symlinkSync(resolve(file!), join(data, `${m}${d}${y}.NASDAQ_ITCH50.bin`));
    server = await serve(['--data', data, '--date', date, '--allow-truncated-tail', ...store]);
    await page.addInitScript(instrument);
    await page.goto(server.url);
    const session = page.getByTestId('session');
    const speed = (name: string) => page.getByRole('button', { name, exact: true }).click();

    // 1. Playing the replay, at each speed.
    await measure(page, 'load symbol', async () => {
      await page.getByLabel('Symbol').fill(symbol);
      await page.getByLabel('Symbol').press('Enter');
      await expect(session).toContainText(`${symbol} · gen 2 · paused`, { timeout: 120_000 });
    });
    await speed('1×');
    await measure(page, 'play 1×', () => page.getByRole('button', { name: '▶ Play' }).click(), 10_000);
    await measure(page, 'play 10×', () => speed('10×'), 10_000);
    await measure(page, 'play 100×', () => speed('100×'), 10_000);
    await measure(page, 'play Max', () => speed('Max'), 10_000);

    // 2. Switching to the Lab and back while it plays at Max.
    await measure(page, 'to Lab while playing', () => page.getByRole('link', { name: 'Strategy Lab' }).click(), 5_000);
    await measure(
      page,
      'to Terminal while playing',
      () => page.getByRole('link', { name: 'Terminal', exact: true }).click(),
      5_000,
    );
    await page.getByRole('link', { name: 'Strategy Lab' }).click();

    // 3. Running the default strategy over the day, then watching it at Max to its verdict.
    await measure(page, 'run backtest', async () => {
      await page.getByLabel('symbol', { exact: true }).fill(symbol);
      await page.getByRole('button', { name: '▶ Run backtest' }).click();
      await expect(page.locator('.card', { hasText: 'Total PnL' }).locator('b')).toBeVisible({ timeout: 600_000 });
    });
    await measure(page, 'watch in terminal', async () => {
      await page.getByRole('button', { name: 'Watch in Terminal' }).click();
      await expect(session).toContainText(`${symbol} · gen 3 · paused`, { timeout: 120_000 });
    });
    await speed('Max');
    await measure(
      page,
      'play the watched run at Max',
      () => page.getByRole('button', { name: '▶ Play' }).click(),
      10_000,
    );

    if (process.env.CHRONOS_LAG_OUT)
      writeFileSync(process.env.CHRONOS_LAG_OUT, `${JSON.stringify(windows, null, 2)}\n`);
  } finally {
    try {
      await server?.stop();
    } finally {
      rmSync(data, { recursive: true, force: true });
    }
  }
});
