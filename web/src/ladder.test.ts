import { describe, expect, it } from 'vitest';
import { ladder } from './ladder';
import type { MyOrder } from './protocol';

const order = (side: 'buy' | 'sell', price: number): MyOrder => ({
  id: 1,
  side,
  price,
  remaining: 100,
  ahead_quantity: 400,
  ahead_orders: 2,
});
const prices = (rows: ReturnType<typeof ladder>) => rows.map((r) => (r === null ? null : r.price));

describe('ladder', () => {
  it('lists depth cents beyond each best, highest first, with each side its size', () => {
    const rows = ladder([[100_000, 300, 1]], [[100_100, 200, 1]], [], 4, 3);
    expect(prices(rows)).toEqual([100_300, 100_200, 100_100, 100_000, 99_900, 99_800]);
    expect(rows.map((r) => [r!.bid, r!.ask])).toEqual([
      [0, 0],
      [0, 0],
      [0, 200],
      [300, 0],
      [0, 0],
      [0, 0],
    ]);
  });

  it('shows the cents inside a narrow spread, and one gap row for a wide one', () => {
    const narrow = ladder([[100_000, 1, 1]], [[100_300, 1, 1]], [], 4, 1);
    expect(prices(narrow)).toEqual([100_300, 100_200, 100_100, 100_000]);
    expect(narrow.map((r) => r!.inside)).toEqual([false, true, true, false]);
    expect(prices(ladder([[100_000, 1, 1]], [[110_000, 1, 1]], [], 4, 1))).toEqual([110_000, null, 100_000]);
  });

  it('keeps a level off the grid, and steps a cent under $1 too, never to a price of zero', () => {
    const offGrid = ladder([[100_050, 5, 1]], [[100_100, 5, 1]], [], 4, 2);
    expect(prices(offGrid)).toEqual([100_200, 100_100, 100_050, 100_000, 99_900]);
    // $0.87 bid, $0.8810 and $0.8812 offered: the levels, whole cents beyond them, and the cent between
    const subPenny = ladder(
      [[8_700, 22, 1]],
      [
        [8_810, 1_823, 1],
        [8_812, 10, 1],
      ],
      [],
      4,
      2,
    );
    expect(prices(subPenny)).toEqual([9_000, 8_900, 8_812, 8_810, 8_800, 8_700, 8_600]);
    expect(subPenny.map((r) => r!.inside)).toEqual([false, false, false, false, true, false, false]);
    expect(prices(ladder([[150, 1, 1]], [[200, 1, 1]], [], 4, 3))).toEqual([400, 300, 200, 150, 100]);
  });

  it('puts our resting orders on their price and side, and has no rows for an empty book', () => {
    const rows = ladder(
      [[100_000, 300, 1]],
      [[100_100, 200, 1]],
      [order('buy', 100_000), order('sell', 100_200)],
      4,
      2,
    );
    expect(rows.map((r) => [r!.myBid.length, r!.myAsk.length])).toEqual([
      [0, 1],
      [0, 0],
      [1, 0],
      [0, 0],
    ]);
    expect(ladder([], [], [], 4, 20)).toEqual([]);
  });

  it('shows every level the snapshot holds and every order of ours, however far from the touch', () => {
    const far = order('buy', 90_000);
    const rows = ladder(
      [
        [100_000, 300, 1],
        [95_000, 50, 1],
      ],
      [[100_100, 200, 1]],
      [far],
      4,
      1,
    );
    expect(prices(rows)).toEqual([100_100, 100_000, 95_000, 90_000]);
    expect(rows.at(-1)!.myBid).toEqual([far]);
  });

  it('handles a one-sided book, a crossed one, and a stub quote across a huge spread at once', () => {
    expect(prices(ladder([[100_000, 1, 1]], [], [], 4, 2))).toEqual([100_000, 99_900]);
    expect(prices(ladder([], [[100_100, 1, 1]], [], 4, 2))).toEqual([100_200, 100_100]);
    const crossed = ladder([[100_100, 5, 1]], [[100_000, 7, 1]], [], 4, 1);
    expect(crossed.map((r) => [r!.price, r!.bid, r!.ask])).toEqual([
      [100_100, 5, 0],
      [100_000, 0, 7],
    ]);
    const t0 = performance.now();
    expect(prices(ladder([[100, 1, 1]], [[1_999_999_900, 1, 1]], [], 4, 1))).toEqual([1_999_999_900, null, 100]);
    expect(performance.now() - t0).toBeLessThan(50);
  });
});
