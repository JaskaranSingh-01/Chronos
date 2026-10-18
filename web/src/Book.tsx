import { clock, px } from './format';
import type { Level, Side } from './protocol';
import { selectOrders, useStore } from './store';

const depth = 10; // levels shown per side

// The order book; clicking a level puts its price in the ticket. Our resting orders show the shares ahead of them.
export function Book({ onPick }: { onPick: (price: string) => void }) {
  const snap = useStore((s) => s.snap);
  const orders = useStore(selectOrders);
  const digits = useStore((s) => s.hello?.price_scale ?? 4);
  const bids = snap?.bids.slice(0, depth) ?? [];
  const asks = snap?.asks.slice(0, depth) ?? [];
  const most = Math.max(1, ...bids.map((l) => l[1]), ...asks.map((l) => l[1]));
  const row = (side: Side) => (level: Level) => {
    const [price, quantity] = level;
    const mine = orders.filter((o) => o.side === side && o.price === price);
    return (
      <div
        key={price}
        className={`level ${side} ${mine.length > 0 ? 'mine' : ''}`}
        onClick={() => onPick(px(price, digits))}
      >
        <span>{px(price, digits)}</span>
        <span>{quantity}</span>
        <span className="depth" style={{ width: `${(100 * quantity) / most}%` }} />
        {mine.map((o) => (
          <span key={o.id} className="badge">
            you {o.remaining} · {o.ahead_quantity} ahead
          </span>
        ))}
      </div>
    );
  };
  return (
    <div className="panel book">
      <div className="title">Order book · click a price to use it</div>
      <div className="asks">{[...asks].reverse().map(row('sell'))}</div>
      <div className="spread">
        {bids[0] && asks[0]
          ? `${px(bids[0][0], digits)} / ${px(asks[0][0], digits)} · spread ${px(asks[0][0] - bids[0][0], digits)}`
          : '—'}
      </div>
      <div>{bids.map(row('buy'))}</div>
    </div>
  );
}

// Market trades, newest first, coloured by the aggressor: the side opposite the resting order.
export function Tape() {
  const tape = useStore((s) => s.tape);
  const digits = useStore((s) => s.hello?.price_scale ?? 4);
  return (
    <div className="panel tape">
      <div className="title">Trades</div>
      {tape
        .slice(-50)
        .reverse()
        .map(([ts, price, quantity, resting], i) => (
          <div key={i} className={`kv ${resting === 'sell' ? 'buy' : 'sell'}`}>
            <span>{clock(ts)}</span>
            <span>{px(price, digits)}</span>
            <span>{quantity}</span>
          </div>
        ))}
    </div>
  );
}
