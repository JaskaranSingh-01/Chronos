import type { Level, MyOrder } from './protocol';

export interface Row {
  price: number; // ticks
  bid: number; // the book's shares at this price on the bid, 0 if none
  ask: number;
  myBid: MyOrder[]; // our resting buys at this price
  myAsk: MyOrder[];
  inside: boolean; // strictly between the best bid and the best ask
}

// The depth ladder's rows, highest price first: `depth` grid prices from each best outwards, every level the snapshot
// holds, each of our resting orders' prices, and the grid prices between the bests (one null row for a spread wider
// than `inside` of them). The grid is a cent at $1 or more and one tick below it, as US equities quote. Empty when
// both sides are.
export function ladder(
  bids: readonly Level[],
  asks: readonly Level[],
  mine: readonly MyOrder[],
  digits: number,
  depth: number,
  inside = 5,
): (Row | null)[] {
  const bestBid = bids[0]?.[0];
  const bestAsk = asks[0]?.[0];
  const reference = bestAsk ?? bestBid;
  if (reference === undefined) return [];
  const step = reference >= 10 ** digits ? 10 ** Math.max(digits - 2, 0) : 1;
  const prices = new Set<number>([...bids, ...asks].map(([p]) => p));
  mine.forEach((o) => prices.add(o.price));
  for (let i = 0; i < depth; i++) {
    if (bestAsk !== undefined) prices.add(bestAsk + i * step);
    if (bestBid !== undefined) prices.add(bestBid - i * step);
  }
  const between = bestBid !== undefined && bestAsk !== undefined ? Math.floor((bestAsk - bestBid - 1) / step) : 0;
  const wide = between > inside;
  if (!wide) for (let k = 1; k <= between; k++) prices.add(bestBid! + k * step);
  const size = (levels: readonly Level[]) => new Map(levels.map(([p, q]) => [p, q]));
  const bidSize = size(bids);
  const askSize = size(asks);
  const row = (price: number): Row => ({
    price,
    bid: bidSize.get(price) ?? 0,
    ask: askSize.get(price) ?? 0,
    myBid: mine.filter((o) => o.side === 'buy' && o.price === price),
    myAsk: mine.filter((o) => o.side === 'sell' && o.price === price),
    inside: bestBid !== undefined && bestAsk !== undefined && price > bestBid && price < bestAsk,
  });
  const rows: (Row | null)[] = [...prices].sort((a, b) => b - a).map(row);
  if (wide)
    rows.splice(
      rows.findIndex((r) => r !== null && r.price <= bestBid!),
      0,
      null,
    );
  return rows;
}
