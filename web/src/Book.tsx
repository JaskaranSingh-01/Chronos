import { clock, px } from './format';
import { ladder } from './ladder';
import { selectOrders, useStore } from './store';

const depth = 20; // grid rows beyond each best: the snapshot's levels a side

// The L3 book as a price ladder, the spread in the middle: our size and the shares ahead of it, the book's size, the
// price. Clicking a row puts its price in the ticket.
export function DepthLadder({ onPick }: { onPick: (price: string) => void }) {
  const bids = useStore((s) => s.snap?.bids);
  const asks = useStore((s) => s.snap?.asks);
  const orders = useStore(selectOrders);
  const digits = useStore((s) => s.hello?.price_scale ?? 4);
  const rows = ladder(bids ?? [], asks ?? [], orders, digits, depth);
  const most = Math.max(1, ...rows.map((r) => Math.max(r?.bid ?? 0, r?.ask ?? 0)));
  const bestBid = bids?.[0]?.[0];
  const bestAsk = asks?.[0]?.[0];
  const bar = (size: number) => <i className="bar-fill" style={{ width: `${(100 * size) / most}%` }} />;
  return (
    <section className="panel ladder" aria-label="Depth ladder">
      <div className="title">
        Depth · L3
        <span className="grow" />
        {bestBid !== undefined && bestAsk !== undefined && (
          <span className="num">
            sp <b>{px(bestAsk - bestBid, digits)}</b> · mid{' '}
            <b>{((bestBid + bestAsk) / 2 / 10 ** digits).toFixed(bestBid >= 10 ** digits ? 3 : digits + 1)}</b>
          </span>
        )}
      </div>
      <div className="head">
        <span>MY BID</span>
        <span>BID</span>
        <span>PRICE</span>
        <span>ASK</span>
        <span>MY ASK</span>
      </div>
      <div className="rows">
        {rows.map((r) =>
          r === null ? (
            <div key="gap" className="row gap">
              ⋯ spread {px((bestAsk ?? 0) - (bestBid ?? 0), digits)} ⋯
            </div>
          ) : (
            <div
              key={r.price}
              className={`row${r.bid > 0 ? ' level buy' : r.ask > 0 ? ' level sell' : ''}${r.inside ? ' inside' : ''}`}
              role="button"
              tabIndex={0}
              aria-label={`Use ${px(r.price, digits)}`}
              onClick={() => onPick(px(r.price, digits))}
              onKeyDown={(e) => (e.key === 'Enter' || e.key === ' ') && onPick(px(r.price, digits))}
            >
              <span className="mine">
                {r.myBid.map((o) => (
                  <b key={o.id}>
                    {o.remaining}/{o.ahead_quantity}
                  </b>
                ))}
              </span>
              <span className="size bid">
                {r.bid > 0 && bar(r.bid)}
                <span>{r.bid > 0 ? r.bid : ''}</span>
              </span>
              <span className="px">{px(r.price, digits)}</span>
              <span className="size ask">
                {r.ask > 0 && bar(r.ask)}
                <span>{r.ask > 0 ? r.ask : ''}</span>
              </span>
              <span className="mine ask">
                {r.myAsk.map((o) => (
                  <b key={o.id}>
                    {o.remaining}/{o.ahead_quantity}
                  </b>
                ))}
              </span>
            </div>
          ),
        )}
      </div>
      <div className="foot">
        <span>
          <b>MY</b> = our shares / shares ahead in the queue
        </span>
      </div>
    </section>
  );
}

// Market trades, newest first, coloured by the aggressor: the side opposite the resting order.
export function Tape() {
  const tape = useStore((s) => s.tape);
  const digits = useStore((s) => s.hello?.price_scale ?? 4);
  return (
    <section className="panel tape" aria-label="Time and sales">
      <div className="title">
        Time &amp; sales
        <span className="grow" />
        <span className="muted">by aggressor</span>
      </div>
      <div className="head">
        <span>TIME</span>
        <span>PX</span>
        <span>SZ</span>
      </div>
      <div className="rows">
        {tape
          .slice(-60)
          .reverse()
          .map(([ts, price, quantity, resting], i) => (
            <div key={i} className={`kv ${resting === 'sell' ? 'buy' : 'sell'}`}>
              <span>{clock(ts)}</span>
              <span>{px(price, digits)}</span>
              <span>{quantity}</span>
            </div>
          ))}
      </div>
    </section>
  );
}
