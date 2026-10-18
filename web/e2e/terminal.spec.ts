import { test as base, expect, type Page } from '@playwright/test';
import { rmSync } from 'node:fs';
import { fixture, serve } from './serve';

// Each test gets its own chronos-serve on the fixture day; `serverArgs` adds options such as --token.
const test = base.extend<{ serverArgs: string[]; url: string }>({
  serverArgs: [[], { option: true }],
  url: async ({ serverArgs }, use) => {
    const data = fixture();
    const server = await serve(['--data', data, '--date', '2019-12-30', ...serverArgs]);
    await use(server.url);
    await server.stop();
    rmSync(data, { recursive: true });
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
  await expect(page.locator('.level.buy')).toHaveText(/10\.00\s*300/);
  await step.click();
  await expect(page.locator('.level.sell')).toHaveText(/10\.01\s*300/);
  await step.click();
  await expect(page.locator('.level.sell')).toHaveText(/10\.01\s*200/);
}

test('an order placed in the terminal fills and shows in Fills and the account', async ({ page, url }) => {
  await page.goto(url);
  await pickAcme(page);
  await stepTheBook(page);
  await page.locator('.level.sell').click();
  await expect(page.getByLabel('Price')).toHaveValue('10.01');
  await page.getByRole('button', { name: 'IOC' }).click();
  await page.getByRole('button', { name: 'Place buy 100 @ 10.01' }).click();
  // Paused, the order waits; the next step delivers it 50 µs later, before the next market event, against the offer.
  await page.getByRole('button', { name: 'Step ⏭' }).click();
  await page.getByRole('button', { name: 'Fills' }).click();
  await expect(page.locator('.tabs tbody tr').nth(1)).toHaveText(/1buy10010\.01taker$/);
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
  await page.getByRole('button', { name: 'Step ⏭' }).click();
  await expect(page.getByRole('button', { name: 'Open orders (1)' })).toBeVisible();

  await page.getByRole('button', { name: '⟲ Restart' }).click();
  await expect(page.getByTestId('session')).toContainText('ACME · gen 3 · paused');
  await expect(page.getByTestId('session')).not.toContainText('viewer');
  await expect(page.getByRole('button', { name: 'Open orders (0)' })).toBeVisible();
  await expect(page.locator('.level')).toHaveCount(0);

  await stepTheBook(page);
  await page.locator('.level.buy').click();
  await page.getByRole('button', { name: 'Place buy 100 @ 10.00' }).click();
  await page.getByRole('button', { name: 'Step ⏭' }).click();
  await expect(page.getByRole('button', { name: 'Open orders (1)' })).toBeVisible();
  expect(sent).toHaveLength(2);
  const commands = sent[1].map((text) => JSON.parse(text)).filter((m) => m.type === 'order' || m.type === 'control');
  expect(commands.map((m) => m.generation)).toEqual(commands.map(() => 3));
  await expect(page.locator('.notice')).toHaveText('');
});

test('a seek lands on the New York time typed', async ({ page, url }) => {
  await page.goto(url);
  await pickAcme(page);
  await page.getByLabel('Seek to').fill('09:31:45');
  await page.getByRole('button', { name: 'Seek' }).click();
  await expect(page.getByTestId('clock')).toHaveText('09:31:45.000 ET');
  await expect(page.getByTestId('session')).toContainText('ACME · gen 2 · paused');
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
  });
});
