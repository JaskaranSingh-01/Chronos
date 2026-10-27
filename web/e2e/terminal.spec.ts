import { test as base, expect, type Page } from '@playwright/test';
import { rmSync } from 'node:fs';
import { fixture, serve } from './serve';

// Each test gets its own chronos-serve on the fixture days; `serverArgs` adds options such as --token.
const test = base.extend<{ serverArgs: string[]; url: string }>({
  serverArgs: [[], { option: true }],
  url: async ({ serverArgs }, use) => {
    const data = fixture();
    try {
      const server = await serve(['--data', data, '--date', '2019-12-30', ...serverArgs]);
      try {
        await use(server.url);
      } finally {
        await server.stop();
      }
    } finally {
      rmSync(data, { recursive: true, force: true });
    }
  },
});

async function pickAcme(page: Page) {
  await page.getByLabel('Symbol').fill('ACME');
  await page.getByLabel('Symbol').press('Enter');
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · paused');
}

// Steps ACME's first three events: 300 bid at 10.00, 300 offered at 10.01, then 100 of the offer trade.
async function stepTheBook(page: Page) {
  const step = page.getByRole('button', { name: 'Step ⏭' });
  await step.click();
  await expect(page.locator('.level.buy')).toHaveText(/300\s*10\.00/); // the ladder: bid size, then price
  await step.click();
  await expect(page.locator('.level.sell')).toHaveText(/10\.01\s*300/);
  await step.click();
  await expect(page.locator('.level.sell')).toHaveText(/10\.01\s*200/);
}

// A step sent before the order reaches the server delivers nothing, so step again until the order rests.
async function stepUntilResting(page: Page) {
  await expect(async () => {
    await page.getByRole('button', { name: 'Step ⏭' }).click();
    await expect(page.getByRole('button', { name: 'Open orders (1)' })).toBeVisible({ timeout: 500 });
  }).toPass();
}

test('an order placed in the terminal fills and shows in Fills and the account', async ({ page, url }) => {
  await page.goto(url);
  const prompt = page.getByText('Type a ticker (e.g. AAPL) in the Symbol box');
  await expect(prompt).toBeVisible();
  await expect(page.getByLabel('Symbol')).toBeFocused();
  await expect(page.getByText('Read-only')).toHaveCount(0);
  // The controller seat is taken, so a second tab is a viewer and says so.
  const second = await page.context().newPage();
  await second.goto(url);
  await expect(second.getByText('Read-only: another window or tab controls this session.')).toBeVisible();
  await expect(second.getByLabel('Symbol')).toBeDisabled();
  await pickAcme(page);
  await expect(prompt).toHaveCount(0);
  await expect(page.getByRole('button', { name: 'Open 09:30' })).toBeEnabled();
  await stepTheBook(page);
  await expect(page.locator('.legend')).toContainText('C 10.01 V');
  await page.locator('.level.sell').click();
  await expect(page.getByLabel('Price')).toHaveValue('10.01');
  await page.getByRole('button', { name: 'IOC' }).click();
  await page.getByRole('button', { name: 'Place buy 100 @ 10.01' }).click();
  await page.getByRole('button', { name: 'Fills' }).click();
  // Paused, the order waits; the next step delivers it 50 µs later, before the next market event, against the offer.
  // As in stepUntilResting, a step that beat the order to the server is repeated.
  await expect(async () => {
    await page.getByRole('button', { name: 'Step ⏭' }).click();
    await expect(page.locator('.tabs tbody tr:not(.muted)')).toHaveText(/1buy10010\.01taker——$/, { timeout: 500 });
  }).toPass();
  await expect(page.getByTestId('Position')).toHaveText('+100');
});

test('a reconnect during a restart ends on the new generation with no stale command', async ({ page, url }) => {
  const sent: string[][] = []; // what the page sent, per connection
  await page.routeWebSocket(/\/ws$/, (ws) => {
    const mine: string[] = [];
    const dropping = sent.push(mine) === 1;
    const server = ws.connectToServer();
    ws.onMessage((m) => {
      mine.push(String(m));
      server.send(m);
    });
    server.onMessage((m) => {
      const message = JSON.parse(String(m));
      // The first connection drops as the restart ends generation 2, before the page hears of generation 3.
      if (dropping && message.kind === 'generation_end' && message.generation === 2) {
        server.close();
        ws.close();
      } else {
        ws.send(m);
      }
    });
  });
  await page.goto(url);
  await pickAcme(page);
  await stepTheBook(page);
  await page.locator('.level.buy').click();
  await page.getByRole('button', { name: 'Place buy 100 @ 10.00' }).click();
  await stepUntilResting(page);

  await page.getByRole('button', { name: '⟲ Restart' }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 3 · paused');
  await expect(page.getByTestId('session')).not.toContainText('viewer');
  await expect(page.getByRole('button', { name: 'Open orders (0)' })).toBeVisible();
  await expect(page.locator('.level')).toHaveCount(0);

  await stepTheBook(page);
  await page.locator('.level.buy').click();
  await page.getByRole('button', { name: 'Place buy 100 @ 10.00' }).click();
  await stepUntilResting(page);
  expect(sent).toHaveLength(2);
  const commands = sent[1].map((text) => JSON.parse(text)).filter((m) => m.type === 'order' || m.type === 'control');
  expect(commands.map((m) => m.generation)).toEqual(commands.map(() => 3));
  await expect(page.locator('.notice')).toHaveText('');
});

test('a seek lands on the New York time typed', async ({ page, url }) => {
  await page.goto(url);
  await pickAcme(page);
  await page.getByRole('button', { name: 'Open 09:30' }).click();
  await expect(page.getByTestId('clock')).toHaveText('09:30:00.000 ET');
  await expect(page.getByRole('button', { name: 'Cancel seek' })).toBeHidden(); // the clock shows a seek's target early
  await page.getByLabel('Seek to').fill('09:31:45');
  await page.getByRole('button', { name: 'Seek', exact: true }).click();
  await expect(page.getByTestId('clock')).toHaveText('09:31:45.000 ET');
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · paused');
});

test('two days load together and play across the night in no time at 100×', async ({ page, url }) => {
  await page.goto(url);
  await page.getByText('2019-12-30', { exact: true }).click(); // the day picker, on the starting day
  await page.getByLabel('2019-12-27').check();
  await expect(page.locator('.days summary')).toHaveText('2 days');
  await pickAcme(page);
  await expect(page.getByLabel('Seek day')).toBeVisible();
  await page.getByRole('button', { name: '100×' }).click();
  await page.getByRole('button', { name: '▶ Play' }).click();
  // Each day's two minutes take 1.2 s at 100×; waiting out the 72 hours between them would take 43 minutes.
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · ended', { timeout: 15_000 });
  await expect(page.getByTestId('clock')).toHaveText('2019-12-30 09:31:59.000 ET');
  await page.getByLabel('Seek day').selectOption('2019-12-27'); // a seek takes a day too
  await page.getByLabel('Seek to').fill('09:31:45');
  await page.getByRole('button', { name: 'Seek', exact: true }).click();
  await expect(page.getByTestId('clock')).toHaveText('2019-12-27 09:31:45.000 ET');
});

test.describe('with a token', () => {
  test.use({ serverArgs: ['--token', 't0k3n'] });

  test('the terminal asks for it once, keeps it in sessionStorage and authenticates the WebSocket', async ({
    page,
    url,
  }) => {
    await page.goto(url);
    await page.getByLabel('Token').fill('t0k3n');
    await page.getByRole('button', { name: 'Connect' }).click();
    await pickAcme(page);
    expect(await page.evaluate(() => sessionStorage.getItem('chronos-token'))).toBe('t0k3n');
    expect(page.url()).not.toContain('t0k3n');
    // The role is not asserted: the old socket may still hold the controller seat.
    await page.reload();
    await expect(page.getByTestId('session')).toContainText('ACME');
    await expect(page.getByLabel('Token')).toHaveCount(0);
  });
});
