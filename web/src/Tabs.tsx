import { useState } from 'react';
import { clock, px, ruleLabel, shares, signed, tone } from './format';
import { selectOrders, store, useStore } from './store';

const names = ['Positions', 'Open orders', 'Fills', 'Queue'] as const;
const fillRows = 200; // ponytail: the last 200 fills, newest first; a longer list needs windowing

// The position, marked at the bid when long and the ask when short; own slices, so other tabs skip snapshot renders.
function Positions({ digits, moneyDigits }: { digits: number; moneyDigits: number }) {
  const symbol = useStore((s) => s.snap?.symbol);
  const a = useStore((s) => s.snap?.account);
  const mark = useStore((s) => (s.snap && s.snap.account.position > 0 ? s.snap.bids[0] : s.snap?.asks[0])?.[0]);
  return (
    <tbody>
      <tr className="muted">
        <td>Symbol</td>
        <td>Qty</td>
        <td>Avg</td>
        <td>Mark</td>
        <td>uPnL</td>
        <td>rPnL</td>
      </tr>
      {a && a.position !== 0 && (
        <tr>
          <td>{symbol}</td>
          <td className={tone(a.position)}>{shares(a.position)}</td>
          <td>{px(Math.round((a.realised - a.cash) / a.position), digits)}</td>
          <td>{mark === undefined ? '—' : px(mark, digits)}</td>
          <td>{signed(a.unrealised, moneyDigits)}</td>
          <td>{signed(a.realised, moneyDigits)}</td>
        </tr>
      )}
    </tbody>
  );
}

// Positions, open orders (with cancel), fills and each resting order's place in the queue.
export function Tabs() {
  const [tab, setTab] = useState<(typeof names)[number]>('Positions');
  const orders = useStore(selectOrders);
  const fills = useStore((s) => s.fills);
  const hello = useStore((s) => s.hello);
  const incomplete = useStore((s) => s.historyIncomplete);
  const digits = hello?.price_scale ?? 4;
  const moneyDigits = digits + (hello?.quantity_scale ?? 0);
  const controller = hello?.role === 'controller';
  return (
    <div className="panel tabs">
      <div className="title">
        {names.map((name) => (
          <button key={name} className={tab === name ? 'on' : ''} onClick={() => setTab(name)}>
            {name === 'Open orders' ? `Open orders (${orders.length})` : name}
          </button>
        ))}
        {incomplete && <span className="sell"> · report history incomplete</span>}
      </div>
      <table>
        {tab === 'Positions' && <Positions digits={digits} moneyDigits={moneyDigits} />}
        {tab === 'Open orders' && (
          <tbody>
            <tr className="muted">
              <td>Id</td>
              <td>Side</td>
              <td>Price</td>
              <td>Remaining</td>
              <td>Ahead</td>
              <td />
            </tr>
            {orders.map((o) => (
              <tr key={o.id}>
                <td>{o.id}</td>
                <td className={o.side}>{o.side}</td>
                <td>{px(o.price, digits)}</td>
                <td>{o.remaining}</td>
                <td>{o.ahead_quantity}</td>
                <td>
                  <button disabled={!controller} onClick={() => store.cancel(o.id)}>
                    Cancel
                  </button>
                </td>
              </tr>
            ))}
          </tbody>
        )}
        {tab === 'Fills' && (
          <tbody>
            <tr className="muted">
              <td>Time</td>
              <td>Order</td>
              <td>Side</td>
              <td>Qty</td>
              <td>Price</td>
              <td>Liquidity</td>
              <td>Rule</td>
              <td>Ahead at entry</td>
            </tr>
            {fills.slice(-fillRows).reverse().map((f) => (
              <tr key={f.seq}>
                <td>{clock(f.exchange_ts)}</td>
                <td>{f.order_id}</td>
                <td className={f.side}>{f.side}</td>
                <td>{f.quantity}</td>
                <td>{px(f.price, digits)}</td>
                <td>{f.liquidity}</td>
                <td>{ruleLabel(f.rule)}</td>
                <td>{f.ahead_at_entry ?? '—'}</td>
              </tr>
            ))}
          </tbody>
        )}
        {tab === 'Queue' && (
          <tbody>
            <tr className="muted">
              <td>Order</td>
              <td>Side</td>
              <td>Price</td>
              <td>Remaining</td>
              <td>Shares ahead</td>
              <td>Orders ahead</td>
            </tr>
            {orders.map((o) => (
              <tr key={o.id}>
                <td>{o.id}</td>
                <td className={o.side}>{o.side}</td>
                <td>{px(o.price, digits)}</td>
                <td>{o.remaining}</td>
                <td>{o.ahead_quantity}</td>
                <td>{o.ahead_orders}</td>
              </tr>
            ))}
          </tbody>
        )}
      </table>
    </div>
  );
}
