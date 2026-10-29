import { test as base, expect, type Page } from '@playwright/test';
import { rmSync } from 'node:fs';
import { fixture, serve } from './serve';

// Each test gets its own chronos-serve on the Lab's fixture days, in a working directory of its own, so the scripts it
// saves start from none.
const test = base.extend<{ url: string }>({
  url: async ({}, use) => {
    const data = fixture(true);
    const server = await serve(['--data', data]);
    try {
      await use(server.url);
    } finally {
      await server.stop();
      rmSync(data, { recursive: true, force: true });
    }
  },
});

// A quote on each side at the touch, logging each fill.
const quoter = `summary = "Quotes both sides at the touch"
local size = param("size", 100, {min = 1, max = 10000, help = "shares a quote"})
limits{max_position = 500, max_order_quantity = size, max_open_orders = 2}
local bid_id, ask_id, fills = nil, nil, 0
function on_market(e)
  if not trading() then return end
  local bid = best_bid()
  local ask = best_ask()
  if bid and not bid_id and position() + size <= 500 then bid_id = buy(bid, size, "post_only") end
  if ask and not ask_id and position() - size >= -500 then ask_id = sell(ask, size, "post_only") end
end
function on_report(r)
  if r.type == "fill" then
    fills = fills + 1
    stat("Fills", fills)
    log("fill", r.side, r.qty, "at", dollars(r.price))
  end
  if r.type ~= "accepted" and r.leaves == 0 then
    if r.id == bid_id then bid_id = nil elseif r.id == ask_id then ask_id = nil end
  end
end
`;

const card = (page: Page, name: string) =>
  page
    .locator('.card')
    .filter({ has: page.getByText(name, { exact: true }) })
    .locator('b');

test('a strategy written in the browser is checked on save, runs in the Lab with its stats and log, and is deleted', async ({
  page,
  url,
}) => {
  await page.goto(`${url}#strategies`);
  await expect(page.getByRole('region', { name: 'Guidelines' })).toContainText('Before you write or test a strategy');
  await expect(page.getByRole('region', { name: 'Scripts' })).toContainText('None yet');

  // A new strategy starts from a template that loads.
  await page.getByLabel('new strategy name').fill('quoter');
  await page.getByRole('button', { name: 'New' }).click();
  const source = page.getByLabel('source');
  await expect(source).toHaveValue(/param\("size", 100/);
  await page.getByRole('button', { name: 'Save' }).click();
  const check = page.getByRole('status');
  await expect(check).toHaveText(
    'Loads. Params: size 100, max_position 500. Limits: 500 position, 100 an order, 2 open.',
  );

  // A broken line is saved, and the check names it.
  await source.fill('function on_market(e)\n  x = = 1\nend\n');
  await expect(page.getByLabel('unsaved')).toBeVisible();
  await page.getByRole('button', { name: 'Save' }).click();
  await expect(check).toHaveText("load: script:2: unexpected symbol near '=' (line 2)");
  await expect(page.getByLabel('unsaved')).toHaveCount(0);
  await expect(page.locator('.scripts li', { hasText: 'quoter' })).toContainText('error');

  // Fixed: it loads, and the Lab offers it with its params.
  await source.fill(quoter);
  await source.press('ControlOrMeta+s');
  await expect(check).toHaveText('Loads. Params: size 100. Limits: 500 position, 100 an order, 2 open.');
  await page.getByRole('link', { name: 'Strategy Lab' }).click();
  await page.getByLabel('strategy', { exact: true }).selectOption('script:quoter');
  await expect(page.getByText('Quotes both sides at the touch')).toBeVisible();
  await expect(page.getByLabel('params.size')).toHaveValue('100');
  await page.getByLabel('params.size').fill('50');
  await page.getByLabel('symbol', { exact: true }).fill('ACME');
  await page.getByRole('button', { name: '▶ Run backtest' }).click();
  await expect(card(page, 'quoter (script) · Fills')).toHaveText(/^[1-9]/);
  await page.getByRole('button', { name: 'Log', exact: true }).click();
  await expect(page.locator('.lab-tabs td.log').first()).toHaveText(/^fill (buy|sell) 50 at \d/);

  // Deleted, it is offered no more.
  await page.getByRole('link', { name: 'Strategies' }).click();
  await page.getByLabel('open quoter').click();
  await expect(source).toHaveValue(quoter);
  await page.getByRole('button', { name: 'Delete' }).click();
  await page.getByRole('button', { name: 'Delete: sure?' }).click();
  await expect(page.getByRole('region', { name: 'Scripts' })).toContainText('None yet');
  await page.getByRole('link', { name: 'Strategy Lab' }).click();
  await expect(page.getByLabel('strategy', { exact: true }).locator('option[value="script:quoter"]')).toHaveCount(0);
});
