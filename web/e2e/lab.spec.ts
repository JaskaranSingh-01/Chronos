import { test as base, expect, type Page } from '@playwright/test';
import { closeSync, openSync, readFileSync, rmSync, writeSync } from 'node:fs';
import { join } from 'node:path';
import { fixture, serve } from './serve';

// Each test gets its own chronos-serve on the Lab's fixture days. On both, ACME gives the default market maker $1 a
// cycle, buying by rule (a) and selling by rule (c): two cycles on 2019-12-27 and three on 2019-12-30, which alone
// lists ZETA, with one bid, and IDLE, with no market event.
const test = base.extend<{ data: string; url: string }>({
  data: async ({}, use) => {
    const dir = fixture(true);
    try {
      await use(dir);
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  },
  url: async ({ data }, use) => {
    const server = await serve(['--data', data]);
    try {
      await use(server.url);
    } finally {
      await server.stop();
    }
  },
});

// Queues the default market maker on ACME over the starting day, 2019-12-30, and `also`, its form edited by `edit`.
async function runAcme(page: Page, also: string[] = [], edit = async () => {}) {
  await page.getByLabel('symbol', { exact: true }).fill('ACME');
  if (also.length > 0) {
    await page.locator('.form .days summary').click();
    for (const day of also) await page.getByLabel(day).check();
    await page.locator('.form .days summary').click();
  }
  await edit();
  await page.getByRole('button', { name: '▶ Run backtest' }).click();
}

const card = (page: Page, name: string) => page.locator('.card', { hasText: name }).locator('b');
const rows = (page: Page) => page.locator('.lab-tabs tbody tr:not(.muted)');
const runs = (page: Page) => page.locator('.runs > table tr:not(.muted)');

test('a one-day run completes with its stat cards and its fills by rule', async ({ page, url }) => {
  await page.goto(`${url}#lab`);
  await expect(page.getByLabel('window')).toHaveValue('regular');
  await expect(page.getByLabel('window').locator('option:checked')).toHaveText('Regular (feed session)');
  await runAcme(page);
  await expect(card(page, 'Total PnL')).toHaveText('+$3.00');
  await expect(card(page, 'Fills')).toHaveText('6');
  await expect(card(page, 'Max drawdown')).toHaveText('$1.00');
  await expect(runs(page)).toHaveText([/1Market makerACME1\+\$3\.00/]);
  await expect(rows(page)).toHaveCount(6);
  await expect(rows(page).first()).toHaveText('09:30:02.000buy10.00100maker(a) E300+100');
  await expect(rows(page).nth(1)).toHaveText('09:30:04.000sell10.01100maker(c) Add3000');
  await page.getByRole('button', { name: 'Fill attribution' }).click();
  await expect(rows(page).first()).toHaveText('(a) E0/00/03/30/00/0');
  await expect(rows(page).nth(4)).toHaveText('(c) Add0/00/03/00/00/0');
});

test("a two-day run shows each day's PnL, and Compare flags it against a one-day run", async ({ page, url }) => {
  await page.goto(`${url}#lab`);
  await runAcme(page);
  await expect(card(page, 'Total PnL')).toHaveText('+$3.00');
  await runAcme(page, ['2019-12-27']);
  await expect(card(page, 'Total PnL')).toHaveText('+$5.00');
  await page.getByRole('button', { name: 'Per-day PnL' }).click();
  await expect(rows(page)).toHaveText(['2019-12-27+$2.004', '2019-12-30+$3.006']);
  // Two runs queued within a second are listed in either order.
  await page.getByLabel('compare #1').check();
  await page.getByLabel('compare #2').check();
  await page.getByRole('button', { name: 'Compare #1 vs #2' }).click();
  await expect(page.getByText('not like-for-like: another symbol or other days')).toBeVisible();
  await expect(rows(page).first()).toHaveText(/^Total PnL(\+\$3\.00\+\$5\.00\+\$2\.00|\+\$5\.00\+\$3\.00-\$2\.00)$/);
  await expect(page.locator('tr.diff')).toHaveText([
    /^days2019-12-(30|27, 2019-12-30)2019-12-(27, 2019-12-30|30)differs$/,
  ]);
});

test("Watch in Terminal prices the ticket by the run's fees, plays at Max and shows the run reproduced", async ({
  page,
  url,
}) => {
  await page.goto(`${url}#lab`);
  await runAcme(page, [], () => page.getByLabel('fees.maker').fill('-0.002'));
  await expect(card(page, 'Total PnL')).toHaveText('+$4.20'); // $3.00 and 600 shares' $0.002 rebates
  // Another tab is a viewer: it may not Watch, and says why.
  const viewer = await page.context().newPage();
  await viewer.goto(`${url}#lab`);
  await runs(viewer).click();
  await expect(viewer.getByRole('button', { name: 'Watch in Terminal' })).toBeDisabled();
  await expect(viewer.getByText('Another tab holds control')).toBeVisible();
  await page.getByRole('button', { name: 'Watch in Terminal' }).click();
  await expect(page.getByTestId('watch')).toContainText('Watching run #1');
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · paused');
  await page.getByLabel('Price').fill('10.00');
  await expect(page.locator('.ticket')).toContainText('Est. rebate+$0.20'); // the run's maker fee, not hello's 0
  await expect(page.locator('.ticket button.place')).toBeDisabled(); // the run's strategy trades, not the hand
  await page.getByRole('button', { name: 'Max', exact: true }).click();
  await page.getByRole('button', { name: '▶ Play' }).click();
  await expect(page.getByTestId('watch')).toHaveText('Watching run #1 · ✓ reproduced', { timeout: 15_000 });
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · ended');
});

test('Verify shows the run reproduced, Export saves its JSON, and Delete removes it once confirmed', async ({
  page,
  url,
}) => {
  await page.goto(`${url}#lab`);
  await runAcme(page);
  await expect(card(page, 'Total PnL')).toHaveText('+$3.00');
  await page.getByRole('button', { name: 'Verify', exact: true }).click();
  await expect(page.locator('.prov .title').nth(1)).toHaveText('Verifications · ✓ reproduced');
  await expect(page.locator('.prov b.buy')).toHaveText(['✓ reproduced']);
  const saved = page.waitForEvent('download');
  await page.getByRole('button', { name: 'Export JSON' }).click();
  const download = await saved;
  const run = JSON.parse(readFileSync(await download.path(), 'utf8'));
  expect(download.suggestedFilename()).toBe(`${run.id}.json`);
  expect([run.results.pnl, run.verifications.map((v: { outcome: string }) => v.outcome)]).toEqual([
    30_000,
    ['reproduced'],
  ]);
  // A double click on Delete only arms it: the second click lands on Confirm delete and is ignored.
  await page.getByRole('button', { name: 'Delete', exact: true }).dblclick();
  await expect(page.getByRole('button', { name: 'Confirm delete' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Keep' })).toBeVisible();
  await expect(runs(page)).toHaveCount(1);
  await page.getByRole('button', { name: 'Confirm delete' }).click();
  await expect(runs(page)).toHaveCount(0);
  await expect(page.getByText('Run a backtest, or pick one from the runs.')).toBeVisible();
});

test('a symbol missing on one day is refused, naming the day; suggestions omit a symbol without events', async ({
  page,
  url,
}) => {
  await page.goto(`${url}#lab`);
  await expect(page.locator('#lab-symbols option')).toHaveCount(2);
  expect(await page.locator('#lab-symbols option').evaluateAll((o) => o.map((e) => e.getAttribute('value')))).toEqual([
    'ACME',
    'ZETA',
  ]);
  await page.getByLabel('symbol', { exact: true }).fill('ZETA');
  await page.locator('.form .days summary').click();
  await page.getByLabel('2019-12-27').check();
  await page.locator('.form .days summary').click();
  await page.getByRole('button', { name: '▶ Run backtest' }).click();
  await expect(page.locator('.form .sell')).toHaveText('ZETA is not listed on 2019-12-27');
  await expect(page.getByLabel('symbol', { exact: true })).toHaveAttribute('aria-invalid', 'true');
  await expect(runs(page)).toHaveCount(0);
});

// Edits one byte of Monday, the market category of `symbol`'s directory entry, in place, and lists the days again,
// which builds Monday into the store again.
async function editDirectory(page: Page, url: string, data: string, symbol: string) {
  const file = join(data, '12302019.NASDAQ_ITCH50.bin');
  const at = readFileSync(file).indexOf(`${symbol}    `) + 8;
  expect(at).toBeGreaterThan(7);
  const fd = openSync(file, 'r+');
  writeSync(fd, 'Q', at);
  closeSync(fd);
  expect((await page.request.get(`${url}api/days`)).status()).toBe(200);
}

test("a day rebuilt with the run's own symbol changed: Verify and Watch are refused as a changed input", async ({
  page,
  url,
  data,
}) => {
  await page.goto(`${url}#lab`);
  await runAcme(page);
  await expect(card(page, 'Total PnL')).toHaveText('+$3.00');
  await editDirectory(page, url, data, 'ACME');
  await page.getByRole('button', { name: 'Verify', exact: true }).click();
  await expect(page.locator('.bar .notice')).toHaveText('verify refused: a day changed since the run');
  await expect(page.locator('.prov .title').nth(1)).toHaveText('Verifications · none on this code and days yet');
  await page.getByRole('button', { name: 'Watch in Terminal' }).click();
  await expect(page.locator('.bar .notice')).toHaveText('bad_request: 2019-12-30: not the day the run replayed');
  await expect(page.getByTestId('watch')).toBeHidden();
});

test("a day rebuilt with another symbol changed: the run's segment is the same, so Verify and Watch reproduce it", async ({
  page,
  url,
  data,
}) => {
  await page.goto(`${url}#lab`);
  await runAcme(page);
  await expect(card(page, 'Total PnL')).toHaveText('+$3.00');
  await editDirectory(page, url, data, 'ZETA');
  await page.getByRole('button', { name: 'Verify', exact: true }).click();
  await expect(page.locator('.prov .title').nth(1)).toHaveText('Verifications · ✓ reproduced');
  await page.getByRole('button', { name: 'Watch in Terminal' }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · paused');
  await page.getByRole('button', { name: 'Max', exact: true }).click();
  await page.getByRole('button', { name: '▶ Play' }).click();
  await expect(page.getByTestId('watch')).toHaveText('Watching run #1 · ✓ reproduced', { timeout: 15_000 });
});

test('Stop watching returns the plain replay; Restart and Seek keep the Watch; a viewer may not stop it', async ({
  page,
  url,
}) => {
  await page.goto(`${url}#lab`);
  await runAcme(page);
  await expect(card(page, 'Total PnL')).toHaveText('+$3.00');
  await page.getByRole('button', { name: 'Watch in Terminal' }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · paused');
  const viewer = await page.context().newPage();
  await viewer.goto(url);
  await expect(viewer.getByTestId('watch')).toContainText('Watching run #1');
  await expect(viewer.getByRole('button', { name: 'Stop watching' })).toBeDisabled();
  await page.getByRole('button', { name: '⟲ Restart' }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 3 · paused');
  await page.getByLabel('Seek to').fill('09:30:20');
  await page.getByRole('button', { name: 'Seek', exact: true }).click();
  await expect(page.getByTestId('clock')).toHaveText('09:30:20.000 ET');
  await page.getByLabel('Seek to').fill('09:30:05'); // back: a new generation
  await page.getByRole('button', { name: 'Seek', exact: true }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 4');
  await expect(page.getByTestId('watch')).toContainText('Watching run #1');
  await page.getByLabel('Price').fill('10.00');
  await expect(page.locator('.ticket button.place')).toBeDisabled(); // the run's strategy trades
  await page.getByRole('button', { name: 'Stop watching' }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 5 · paused');
  await expect(page.getByTestId('watch')).toBeHidden();
  await expect(page.locator('.ticket button.place')).toBeEnabled();
});
